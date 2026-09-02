#include "floppy.h"
#include "flux_read.pio.h"
#include "flux_write.pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "mfm.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include <string.h>

#define DIR_INWARD 1
#define DIR_OUTWARD 2
#define FLOPPY_FLUX_DMA_COUNT 0x0FFFFFFFu
#define FLOPPY_READ_REVOLUTIONS 4u
#define FLOPPY_READ_TRANSITIONS_MAX 500000u
#define FLOPPY_READ_DEADLINE_MS 5000u
#define FLOPPY_WRITE_DEADLINE_MS 20000u
#define FLOPPY_CONTROL_DEADLINE_MS 7000u
#define FLOPPY_WRITE_ATTEMPTS 3u
#define FLOPPY_HEAD_SETTLE_MS 20u
#define FLOPPY_MOTOR_DEADLINE_MS 2000u
#define FLOPPY_INDEX_PERIOD_MIN_MS 150u
#define FLOPPY_INDEX_PERIOD_MAX_MS 250u
#define FLOPPY_INDEX_PERIOD_SLOP_MS 25u
#define FLOPPY_TX_FIFO_DEPTH 8u
#define FLOPPY_TX_POLL_INTERVAL 32u
#define FLOPPY_WRITE_TRANSITIONS_MAX 200000u
#define FLOPPY_LIFECYCLE 0x464C5059u

#ifndef NUM_BANK0_GPIOS
#define NUM_BANK0_GPIOS 30u
#endif

#ifndef FLOPPY_FLUX_WAIT_TIMEOUT_MS
#define FLOPPY_FLUX_WAIT_TIMEOUT_MS 250u
#endif

#ifndef FLOPPY_INDEX_TIMEOUT_MS
#define FLOPPY_INDEX_TIMEOUT_MS 500u
#endif

#ifndef FLOPPY_WRITE_FLUX_TIMEOUT_MS
#define FLOPPY_WRITE_FLUX_TIMEOUT_MS 1000u
#endif

#ifndef FLOPPY_STEP_MS
#define FLOPPY_STEP_MS 6u
#endif

typedef struct {
  uint32_t start_ms;
  uint32_t limit_ms;
} floppy_deadline_t;

static floppy_t *active_floppy;

static uint32_t now_ms(void) {
  return (uint32_t)to_ms_since_boot(get_absolute_time());
}

static bool elapsed(uint32_t start, uint32_t limit) {
  return (uint32_t)(now_ms() - start) >= limit;
}

static uint32_t remaining(uint32_t start, uint32_t limit) {
  uint32_t used = (uint32_t)(now_ms() - start);
  return used >= limit ? 0 : limit - used;
}

static floppy_deadline_t deadline_begin(uint32_t limit_ms) {
  return (floppy_deadline_t){.start_ms = now_ms(), .limit_ms = limit_ms};
}

static uint32_t deadline_remaining(const floppy_deadline_t *deadline) {
  return remaining(deadline->start_ms, deadline->limit_ms);
}

static bool deadline_elapsed(const floppy_deadline_t *deadline) {
  return deadline_remaining(deadline) == 0;
}

static uint32_t next_generation(uint32_t generation) {
  generation++;
  return generation == 0 ? 1 : generation;
}

static bool floppy_ready(const floppy_t *f) {
  return f && f == active_floppy && f->lifecycle == FLOPPY_LIFECYCLE;
}

static uint floppy_dma_channel(const floppy_t *f) {
  return (uint)f->dma_ch;
}

static disk_err_t floppy_operation_begin(floppy_t *f, floppy_operation_t operation,
                                         uint32_t limit_ms,
                                         floppy_deadline_t *deadline) {
  if (!floppy_ready(f) || !deadline) return DISK_ERR_INVALID;
  if (f->operation != FLOPPY_OPERATION_IDLE) return DISK_ERR_BUSY;
  *deadline = deadline_begin(limit_ms);
  f->operation = operation;
  f->operation_start_ms = deadline->start_ms;
  f->operation_limit_ms = deadline->limit_ms;
  return DISK_OK;
}

static void floppy_operation_end(floppy_t *f, floppy_operation_t operation) {
  if (f->operation == operation) {
    f->operation = FLOPPY_OPERATION_IDLE;
    f->operation_start_ms = 0;
    f->operation_limit_ms = 0;
  }
}

#define FLOPPY_PIN_LIST(p) \
  {(p).index, (p).track0, (p).write_protect, (p).read_data, (p).disk_change, \
   (p).drive_select, (p).motor_enable, (p).direction, (p).step, \
   (p).write_data, (p).write_gate, (p).side_select, (p).density}

static bool floppy_pins_valid(const floppy_pins_t *pins) {
  const uint8_t values[] = FLOPPY_PIN_LIST(*pins);
  for (size_t i = 0; i < sizeof(values); i++) {
    if (values[i] >= NUM_BANK0_GPIOS) return false;
    for (size_t j = 0; j < i; j++) {
      if (values[i] == values[j]) return false;
    }
  }
  return pins->index < 32u && pins->read_data < 32u && pins->write_data < 32u;
}

static void floppy_pin_oc(uint pin, bool high) {
  if (high) {
    gpio_set_dir(pin, GPIO_IN);
  } else {
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_OUT);
  }
}

static void floppy_update_media(floppy_t *f) {
  if (!f->selected) return;
  bool active = !gpio_get(f->pins.disk_change);
  bool initial = !f->media_observed;
  f->media_observed = true;
  if (active && !f->disk_change_active) {
    f->disk_change_active = true;
    f->track0_confirmed = false;
    f->motor_qualified = false;
    if (initial) return;
    f->media_generation = next_generation(f->media_generation);
    f->stats.media_changes++;
  } else if (!active) {
    f->disk_change_active = false;
  }
}

static disk_err_t floppy_flux_media_status(floppy_t *f, uint32_t generation) {
  if (gpio_get(f->pins.disk_change)) return DISK_OK;
  floppy_update_media(f);
  return f->media_generation == generation && !f->disk_change_active
             ? DISK_OK
             : DISK_ERR_MEDIA_CHANGED;
}

static void floppy_stop(floppy_t *f) {
  floppy_pin_oc(f->pins.motor_enable, true);
  f->motor_on = false;
  f->motor_qualified = false;
  floppy_pin_oc(f->pins.drive_select, true);
  f->selected = false;
}

disk_err_t floppy_poll(floppy_t *f) {
  if (!floppy_ready(f)) return DISK_ERR_INVALID;
  if (f->motor_on && f->operation == FLOPPY_OPERATION_IDLE &&
      elapsed(f->last_io_time_ms, FLOPPY_IDLE_TIMEOUT_MS)) {
    floppy_stop(f);
  }
  return DISK_OK;
}

static void floppy_touch(floppy_t *f) {
  f->last_io_time_ms = now_ms();
}

static disk_err_t floppy_wait_index_falling(floppy_t *f,
                                            const floppy_deadline_t *deadline,
                                            uint32_t timeout,
                                            uint32_t generation) {
  uint32_t start = now_ms();
  while (!gpio_get(f->pins.index)) {
    floppy_update_media(f);
    if (f->media_generation != generation) return DISK_ERR_MEDIA_CHANGED;
    if (elapsed(start, timeout) || deadline_elapsed(deadline)) return DISK_ERR_TIMEOUT;
    tight_loop_contents();
  }
  while (gpio_get(f->pins.index)) {
    floppy_update_media(f);
    if (f->media_generation != generation) return DISK_ERR_MEDIA_CHANGED;
    if (elapsed(start, timeout) || deadline_elapsed(deadline)) return DISK_ERR_TIMEOUT;
    tight_loop_contents();
  }
  return DISK_OK;
}

static disk_err_t floppy_wait_stable_index(floppy_t *f,
                                           const floppy_deadline_t *deadline,
                                           uint32_t generation) {
  uint32_t start = now_ms();
  uint32_t previous = 0;
  uint32_t previous_period = 0;
  unsigned stable = 0;
  for (;;) {
    uint32_t left = remaining(start, FLOPPY_MOTOR_DEADLINE_MS);
    uint32_t operation_left = deadline_remaining(deadline);
    if (left > operation_left) left = operation_left;
    if (left == 0) break;
    disk_err_t status = floppy_wait_index_falling(f, deadline, left, generation);
    if (status != DISK_OK) return status;
    uint32_t now = now_ms();
    if (previous != 0 || now == 0) {
      uint32_t period = now - previous;
      bool range = period >= FLOPPY_INDEX_PERIOD_MIN_MS &&
                   period <= FLOPPY_INDEX_PERIOD_MAX_MS;
      uint32_t difference = period > previous_period ? period - previous_period
                                                     : previous_period - period;
      bool consistent = previous_period == 0 ||
                        difference <= FLOPPY_INDEX_PERIOD_SLOP_MS;
      stable = range && consistent ? stable + 1u : 0u;
      previous_period = period;
      if (stable >= 2u) return DISK_OK;
    }
    previous = now;
  }
  return DISK_ERR_TIMEOUT;
}

static disk_err_t floppy_select_internal(floppy_t *f, bool on,
                                         const floppy_deadline_t *deadline) {
  if (f->selected == on) return DISK_OK;
  floppy_pin_oc(f->pins.drive_select, !on);
  f->selected = on;
  sleep_ms(10);
  floppy_touch(f);
  return deadline_elapsed(deadline) ? DISK_ERR_TIMEOUT : DISK_OK;
}

static void floppy_motor_line_on(floppy_t *f) {
  if (!f->motor_on) {
    floppy_pin_oc(f->pins.motor_enable, false);
    f->motor_on = true;
    f->motor_qualified = false;
  }
  floppy_touch(f);
}

static disk_err_t floppy_motor_off_internal(floppy_t *f) {
  if (!f->motor_on) return DISK_OK;
  floppy_pin_oc(f->pins.motor_enable, true);
  f->motor_on = false;
  f->motor_qualified = false;
  floppy_touch(f);
  return DISK_OK;
}

static disk_err_t floppy_qualify_motor(floppy_t *f,
                                       const floppy_deadline_t *deadline,
                                       uint32_t generation) {
  if (f->motor_qualified && f->motor_generation == generation) return DISK_OK;
  disk_err_t status = floppy_wait_stable_index(f, deadline, generation);
  if (status != DISK_OK) return status;
  floppy_update_media(f);
  if (f->media_generation != generation || f->disk_change_active) {
    return DISK_ERR_MEDIA_CHANGED;
  }
  f->motor_generation = generation;
  f->motor_qualified = true;
  return DISK_OK;
}

static disk_err_t floppy_clear_media_latch(floppy_t *f,
                                           const floppy_deadline_t *deadline,
                                           uint32_t generation);

static disk_err_t floppy_prepare(floppy_t *f, uint32_t expected_generation,
                                 const floppy_deadline_t *deadline) {
  floppy_update_media(f);
  if (f->media_generation != expected_generation) return DISK_ERR_MEDIA_CHANGED;
  floppy_touch(f);
  disk_err_t status = floppy_select_internal(f, true, deadline);
  if (status != DISK_OK) return status;
  floppy_motor_line_on(f);
  status = floppy_clear_media_latch(f, deadline, expected_generation);
  if (status != DISK_OK) return status;
  return floppy_qualify_motor(f, deadline, expected_generation);
}

static inline uint32_t flux_ring_produced(floppy_t *f) {
  return FLOPPY_FLUX_DMA_COUNT -
         dma_channel_hw_addr(floppy_dma_channel(f))->transfer_count;
}

static uint32_t pio_rx_stall_mask(const floppy_t *f) {
  return 1u << (PIO_FDEBUG_RXSTALL_LSB + f->read.sm);
}

static uint32_t pio_tx_stall_mask(const floppy_t *f) {
  return 1u << (PIO_FDEBUG_TXSTALL_LSB + f->write.sm);
}

static void pio_clear_stall(PIO pio, uint32_t mask) {
#ifdef PIO_TEST_STUB
  pio->fdebug &= ~mask;
#else
  pio->fdebug = mask;
#endif
}

static bool pio_stalled(PIO pio, uint32_t mask) {
  return (pio->fdebug & mask) != 0;
}

static inline bool flux_data_available(floppy_t *f) {
  return f->read.half_valid || flux_ring_produced(f) != f->ring_cpu;
}

static uint16_t flux_read_sample(floppy_t *f) {
  if (f->read.half_valid) {
    f->read.half_valid = false;
    return f->read.half;
  }
  uint32_t produced = flux_ring_produced(f);
  uint32_t lag = produced - f->ring_cpu;
  if (lag > FLOPPY_FLUX_RING_WORDS) {
    f->stats.overruns++;
    f->read_overrun = true;
    f->ring_cpu = produced - FLOPPY_FLUX_RING_WORDS;
    lag = FLOPPY_FLUX_RING_WORDS;
  }
  if (lag > f->stats.ring_peak) f->stats.ring_peak = lag;
  f->stats.flux_words++;
  uint32_t packed = f->flux_ring[f->ring_cpu & (FLOPPY_FLUX_RING_WORDS - 1u)];
  f->ring_cpu++;
  f->read.half = (uint16_t)(packed >> 16u);
  f->read.half_valid = true;
  return (uint16_t)(packed & 0xFFFFu);
}

static disk_err_t flux_read_wait(floppy_t *f, const floppy_deadline_t *deadline,
                                 uint32_t generation, uint16_t *out) {
  if (!flux_data_available(f)) {
    uint32_t start = now_ms();
    while (!flux_data_available(f)) {
      disk_err_t status = floppy_flux_media_status(f, generation);
      if (status != DISK_OK) return status;
      if (pio_stalled(f->read.pio, pio_rx_stall_mask(f))) {
        f->stats.overruns++;
        return DISK_ERR_OVERRUN;
      }
      if (f->read_overrun) return DISK_ERR_OVERRUN;
      if (elapsed(start, FLOPPY_FLUX_WAIT_TIMEOUT_MS) || deadline_elapsed(deadline)) {
        return DISK_ERR_TIMEOUT;
      }
      tight_loop_contents();
    }
  }
  *out = flux_read_sample(f);
  if (pio_stalled(f->read.pio, pio_rx_stall_mask(f))) {
    f->stats.overruns++;
    return DISK_ERR_OVERRUN;
  }
  disk_err_t status = floppy_flux_media_status(f, generation);
  if (status != DISK_OK) return status;
  return f->read_overrun ? DISK_ERR_OVERRUN : DISK_OK;
}

static void floppy_flux_read_start(floppy_t *f) {
  pio_sm_set_enabled(f->read.pio, f->read.sm, false);
  dma_channel_abort(floppy_dma_channel(f));
  pio_sm_clear_fifos(f->read.pio, f->read.sm);
  pio_sm_restart(f->read.pio, f->read.sm);
  f->read.half_valid = false;
  f->read.primed = false;
  f->read_overrun = false;
  f->ring_cpu = 0;
  pio_sm_exec(f->read.pio, f->read.sm, pio_encode_set(pio_x, 0));
  pio_sm_exec(f->read.pio, f->read.sm, pio_encode_jmp(f->read.offset));
  pio_clear_stall(f->read.pio, pio_rx_stall_mask(f));
  dma_channel_config config = dma_channel_get_default_config(floppy_dma_channel(f));
  channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
  channel_config_set_read_increment(&config, false);
  channel_config_set_write_increment(&config, true);
  channel_config_set_ring(&config, true, FLOPPY_FLUX_RING_BITS);
  channel_config_set_dreq(&config, pio_get_dreq(f->read.pio, f->read.sm, false));
  channel_config_set_high_priority(&config, true);
  dma_channel_configure(floppy_dma_channel(f), &config, f->flux_ring,
                        &f->read.pio->rxf[f->read.sm], FLOPPY_FLUX_DMA_COUNT, true);
  pio_sm_set_enabled(f->read.pio, f->read.sm, true);
}

static void floppy_flux_read_stop(floppy_t *f) {
  pio_sm_set_enabled(f->read.pio, f->read.sm, false);
  dma_channel_abort(floppy_dma_channel(f));
  f->flux_active = false;
}

static disk_err_t floppy_flux_session_start(floppy_t *f, uint32_t expected_generation) {
  if (f->flux_active) return DISK_ERR_INVALID;
  floppy_update_media(f);
  if (f->media_generation != expected_generation || f->disk_change_active) {
    return DISK_ERR_MEDIA_CHANGED;
  }
  floppy_flux_read_start(f);
  f->flux_generation = expected_generation;
  f->flux_active = true;
  return DISK_OK;
}

disk_err_t floppy_flux_begin(floppy_t *f, uint32_t expected_generation) {
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_RAW, FLOPPY_READ_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  status = floppy_prepare(f, expected_generation, &deadline);
  if (status == DISK_OK) status = floppy_flux_session_start(f, expected_generation);
  if (status != DISK_OK) floppy_operation_end(f, FLOPPY_OPERATION_RAW);
  return status;
}

static floppy_pulse_t flux_pulse(floppy_t *f, uint16_t value) {
  uint16_t count = value >> 1;
  int difference = f->read.prev - count;
  if (difference < 0) difference += 0x8000;
  f->read.prev = count;
  return (floppy_pulse_t){.delta = (uint16_t)difference, .index = (value & 1u) != 0};
}

static disk_err_t floppy_flux_next_internal(floppy_t *f,
                                            const floppy_deadline_t *deadline,
                                            uint32_t generation,
                                            floppy_pulse_t *pulse) {
  uint16_t value;
  if (!f->read.primed) {
    disk_err_t status = flux_read_wait(f, deadline, generation, &value);
    if (status != DISK_OK) return status;
    f->read.prev = value >> 1;
    f->read.primed = true;
  }
  disk_err_t status = flux_read_wait(f, deadline, generation, &value);
  if (status != DISK_OK) return status;
  *pulse = flux_pulse(f, value);
  return DISK_OK;
}

disk_result_t floppy_flux_read(floppy_t *f, floppy_pulse_t *pulses, size_t capacity) {
  disk_result_t result = {.error = DISK_OK, .count = 0};
  if (!floppy_ready(f) || f->operation != FLOPPY_OPERATION_RAW || !f->flux_active ||
      !pulses || capacity == 0) {
    result.error = DISK_ERR_INVALID;
    return result;
  }
  floppy_deadline_t deadline = {
      .start_ms = f->operation_start_ms,
      .limit_ms = f->operation_limit_ms,
  };
  result.error = floppy_flux_next_internal(f, &deadline, f->flux_generation, pulses);
  if (result.error != DISK_OK) return result;
  result.count = 1;
  while (result.count < capacity && flux_data_available(f)) {
    uint16_t value = flux_read_sample(f);
    if (f->read_overrun) {
      result.error = DISK_ERR_OVERRUN;
      return result;
    }
    pulses[result.count++] = flux_pulse(f, value);
  }
  if (result.count == 1) return result;
  if (pio_stalled(f->read.pio, pio_rx_stall_mask(f))) {
    f->stats.overruns++;
    result.error = DISK_ERR_OVERRUN;
    return result;
  }
  result.error = floppy_flux_media_status(f, f->flux_generation);
  return result;
}

disk_err_t floppy_flux_end(floppy_t *f) {
  if (!floppy_ready(f) || f->operation != FLOPPY_OPERATION_RAW || !f->flux_active) {
    return DISK_ERR_INVALID;
  }
  floppy_flux_read_stop(f);
  floppy_operation_end(f, FLOPPY_OPERATION_RAW);
  return DISK_OK;
}

static disk_err_t floppy_side_select_internal(floppy_t *f, uint8_t head,
                                              const floppy_deadline_t *deadline) {
  if (head >= DISK_HEADS) return DISK_ERR_INVALID;
  bool changed = !f->head_confirmed || f->head != head;
  floppy_pin_oc(f->pins.side_select, head == 0);
  f->head = head;
  f->head_confirmed = true;
  if (changed) sleep_ms(FLOPPY_HEAD_SETTLE_MS);
  floppy_touch(f);
  return deadline_elapsed(deadline) ? DISK_ERR_TIMEOUT : DISK_OK;
}

static disk_err_t floppy_step_internal(floppy_t *f, int direction,
                                       const floppy_deadline_t *deadline,
                                       uint32_t generation) {
  if (deadline_elapsed(deadline)) return DISK_ERR_TIMEOUT;
  floppy_pin_oc(f->pins.direction, direction != DIR_INWARD);
  sleep_us(10);
  floppy_pin_oc(f->pins.step, false);
  sleep_us(10);
  floppy_pin_oc(f->pins.step, true);
  sleep_ms(FLOPPY_STEP_MS);
  if (direction == DIR_INWARD && f->cylinder + 1u < DISK_CYLINDERS) {
    f->cylinder++;
  } else if (direction == DIR_OUTWARD && f->cylinder > 0) {
    f->cylinder--;
  }
  floppy_update_media(f);
  if (f->media_generation != generation) return DISK_ERR_MEDIA_CHANGED;
  return deadline_elapsed(deadline) ? DISK_ERR_TIMEOUT : DISK_OK;
}

static disk_err_t floppy_clear_media_latch(floppy_t *f,
                                           const floppy_deadline_t *deadline,
                                           uint32_t generation) {
  floppy_update_media(f);
  if (!f->disk_change_active) return DISK_OK;
  disk_err_t status = floppy_step_internal(f, DIR_INWARD, deadline, generation);
  if (status != DISK_OK) return status;
  status = floppy_step_internal(f, DIR_OUTWARD, deadline, generation);
  if (status != DISK_OK) return status;
  sleep_ms(FLOPPY_HEAD_SETTLE_MS);
  if (deadline_elapsed(deadline)) return DISK_ERR_TIMEOUT;
  floppy_update_media(f);
  if (f->media_generation != generation || f->disk_change_active) {
    return DISK_ERR_MEDIA_CHANGED;
  }
  f->track0_confirmed = false;
  return DISK_OK;
}

static disk_err_t floppy_seek_track0_internal(floppy_t *f,
                                              const floppy_deadline_t *deadline,
                                              uint32_t generation, bool *moved) {
  f->track0_confirmed = false;
  for (unsigned step = 0; step < 90u; step++) {
    if (!gpio_get(f->pins.track0)) {
      f->cylinder = 0;
      f->track0_confirmed = true;
      return DISK_OK;
    }
    disk_err_t status = floppy_step_internal(f, DIR_OUTWARD, deadline, generation);
    if (status != DISK_OK) return status;
    *moved = true;
  }
  return DISK_ERR_NO_TRACK0;
}

static disk_err_t floppy_seek_internal(floppy_t *f, uint8_t target,
                                       const floppy_deadline_t *deadline,
                                       uint32_t generation) {
  if (target >= DISK_CYLINDERS) return DISK_ERR_WRONG_TRACK;
  bool moved = false;
  if (!f->track0_confirmed) {
    disk_err_t status = floppy_seek_track0_internal(f, deadline, generation, &moved);
    if (status != DISK_OK) return status;
  }
  while (f->cylinder < target) {
    disk_err_t status = floppy_step_internal(f, DIR_INWARD, deadline, generation);
    if (status != DISK_OK) return status;
    moved = true;
  }
  while (f->cylinder > target) {
    disk_err_t status = floppy_step_internal(f, DIR_OUTWARD, deadline, generation);
    if (status != DISK_OK) return status;
    moved = true;
  }
  if (moved) sleep_ms(FLOPPY_HEAD_SETTLE_MS);
  floppy_touch(f);
  return deadline_elapsed(deadline) ? DISK_ERR_TIMEOUT : DISK_OK;
}

disk_err_t floppy_select(floppy_t *f, bool on) {
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  status = floppy_select_internal(f, on, &deadline);
  floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}

disk_err_t floppy_motor_on(floppy_t *f) {
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  status = floppy_select_internal(f, true, &deadline);
  if (status == DISK_OK) {
    floppy_update_media(f);
    uint32_t generation = f->media_generation;
    floppy_motor_line_on(f);
    status = floppy_wait_stable_index(f, &deadline, generation);
    if (status == DISK_OK && !f->disk_change_active) {
      f->motor_generation = generation;
      f->motor_qualified = true;
    }
  }
  if (status != DISK_OK) floppy_motor_off_internal(f);
  floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}

disk_err_t floppy_motor_off(floppy_t *f) {
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  status = floppy_motor_off_internal(f);
  floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}

disk_err_t floppy_side_select(floppy_t *f, uint8_t head) {
  if (head >= DISK_HEADS) return DISK_ERR_INVALID;
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  status = floppy_side_select_internal(f, head, &deadline);
  floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}

disk_err_t floppy_seek(floppy_t *f, uint8_t target) {
  if (target >= DISK_CYLINDERS) return DISK_ERR_WRONG_TRACK;
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  status = floppy_select_internal(f, true, &deadline);
  if (status == DISK_OK) {
    floppy_update_media(f);
    uint32_t generation = f->media_generation;
    status = floppy_prepare(f, generation, &deadline);
    if (status == DISK_OK) {
      status = floppy_seek_internal(f, target, &deadline, generation);
    }
  }
  floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}

disk_err_t floppy_current_track(const floppy_t *f, uint8_t *cylinder) {
  if (!floppy_ready(f) || !cylinder) return DISK_ERR_INVALID;
  if (!f->track0_confirmed) return DISK_ERR_NO_TRACK0;
  *cylinder = f->cylinder;
  return DISK_OK;
}

disk_err_t floppy_at_track0(const floppy_t *f, bool *active) {
  if (!floppy_ready(f) || !active) return DISK_ERR_INVALID;
  *active = !gpio_get(f->pins.track0);
  return DISK_OK;
}

disk_err_t floppy_disk_changed(floppy_t *f, bool *changed) {
  if (!floppy_ready(f) || !changed) return DISK_ERR_INVALID;
  floppy_update_media(f);
  *changed = f->disk_change_active;
  return DISK_OK;
}

disk_err_t floppy_media_generation(floppy_t *f, uint32_t *generation) {
  if (!floppy_ready(f) || !generation) return DISK_ERR_INVALID;
  floppy_update_media(f);
  *generation = f->media_generation;
  return DISK_OK;
}

static bool floppy_write_protected_raw(const floppy_t *f) {
  return !gpio_get(f->pins.write_protect);
}

disk_err_t floppy_write_protected(const floppy_t *f, bool *write_protected) {
  if (!floppy_ready(f) || !write_protected) return DISK_ERR_INVALID;
  *write_protected = floppy_write_protected_raw(f);
  return DISK_OK;
}

disk_err_t floppy_stats_reset(floppy_t *f) {
  if (!floppy_ready(f)) return DISK_ERR_INVALID;
  if (f->operation != FLOPPY_OPERATION_IDLE) return DISK_ERR_BUSY;
  memset(&f->stats, 0, sizeof(f->stats));
  return DISK_OK;
}

disk_err_t floppy_stats(const floppy_t *f, floppy_stats_t *stats) {
  if (!floppy_ready(f) || !stats) return DISK_ERR_INVALID;
  if (f->operation != FLOPPY_OPERATION_IDLE) return DISK_ERR_BUSY;
  *stats = f->stats;
  return DISK_OK;
}

static bool floppy_status_retryable(disk_err_t status) {
  return status == DISK_ERR_TIMEOUT || status == DISK_ERR_CRC ||
         status == DISK_ERR_OVERRUN || status == DISK_ERR_UNDERRUN ||
         status == DISK_ERR_VERIFY || status == DISK_ERR_WRONG_TRACK;
}

static void floppy_stats_failure(floppy_t *f, disk_err_t status) {
  if (status == DISK_OK) return;
  f->stats.failed++;
  if (status == DISK_ERR_TIMEOUT) f->stats.timeout++;
  else if (status == DISK_ERR_CRC) f->stats.crc++;
  else if (status == DISK_ERR_WRONG_TRACK) f->stats.wrong_track++;
  else if (status == DISK_ERR_WRONG_SIDE) f->stats.wrong_side++;
}

static disk_err_t floppy_jog(floppy_t *f, uint8_t cylinder, uint8_t distance,
                             const floppy_deadline_t *deadline,
                             uint32_t generation) {
  floppy_touch(f);
  uint8_t away = cylinder <= distance ? cylinder + distance : cylinder - distance;
  disk_err_t status = floppy_seek_internal(f, away, deadline, generation);
  if (status != DISK_OK) return status;
  return floppy_seek_internal(f, cylinder, deadline, generation);
}

typedef struct {
  track_t *track;
  const track_t *expected;
  uint8_t cylinder;
  uint8_t head;
  uint32_t seen;
  uint32_t conflict;
  bool verify_failed;
  bool correct_ch;
  bool wrong_track;
  bool wrong_side;
} read_goal_t;

static void read_goal_accept(read_goal_t *goal, const mfm_sector_t *sector) {
  if (sector->cylinder != goal->cylinder) {
    goal->wrong_track = true;
    return;
  }
  if (sector->head != goal->head) {
    goal->wrong_side = true;
    return;
  }
  goal->correct_ch = true;
  if (!disk_sector_valid(sector->sector)) return;
  uint32_t bit = 1u << sector->sector;
  if (goal->seen & bit) {
    goal->conflict |= bit;
    if (goal->expected) goal->verify_failed = true;
    else goal->track->valid &= ~bit;
    return;
  }
  goal->seen |= bit;
  if (goal->expected) {
    if (memcmp(goal->expected->data[sector->sector], sector->data,
               DISK_SECTOR_SIZE) != 0) {
      goal->verify_failed = true;
    }
    return;
  }
  if (goal->conflict & bit) return;
  if (track_has(goal->track, sector->sector)) {
    if (memcmp(goal->track->data[sector->sector], sector->data,
               DISK_SECTOR_SIZE) != 0) {
      goal->track->valid &= ~bit;
      goal->conflict |= bit;
    }
  } else {
    memcpy(goal->track->data[sector->sector], sector->data, DISK_SECTOR_SIZE);
    track_mark(goal->track, sector->sector);
  }
}

static bool read_goal_complete(const read_goal_t *goal) {
  return goal->expected ? goal->seen == DISK_TRACK_VALID
                        : goal->track->valid == DISK_TRACK_VALID;
}

static disk_err_t read_goal_verdict(const read_goal_t *goal, const mfm_t *decoder) {
  if (goal->conflict) return goal->expected ? DISK_ERR_VERIFY : DISK_ERR_CORRUPT;
  if (goal->verify_failed) return DISK_ERR_VERIFY;
  if (decoder->crc_errors > 0) return DISK_ERR_CRC;
  if (decoder->format_errors > 0) return DISK_ERR_CORRUPT;
  if (goal->expected && goal->correct_ch) return DISK_ERR_VERIFY;
  if (!goal->correct_ch && goal->wrong_track) return DISK_ERR_WRONG_TRACK;
  if (!goal->correct_ch && goal->wrong_side) return DISK_ERR_WRONG_SIDE;
  return DISK_ERR_TIMEOUT;
}

static disk_err_t floppy_read_flux(floppy_t *f, uint32_t expected_generation,
                                   read_goal_t *goal, unsigned clean_revolutions,
                                   const floppy_deadline_t *deadline) {
  floppy_update_media(f);
  if (f->media_generation != expected_generation) return DISK_ERR_MEDIA_CHANGED;
  disk_err_t status = floppy_prepare(f, expected_generation, deadline);
  if (status != DISK_OK) return status;
  if (f->media_generation != expected_generation) return DISK_ERR_MEDIA_CHANGED;
  status = floppy_seek_internal(f, goal->cylinder, deadline, expected_generation);
  if (status != DISK_OK) return status;
  status = floppy_side_select_internal(f, goal->head, deadline);
  if (status != DISK_OK) return status;
  status = floppy_flux_session_start(f, expected_generation);
  if (status != DISK_OK) return status;

  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  goal->seen = 0;
  uint32_t transitions = 0;
  uint32_t completed_revolutions = 0;
  unsigned clean = 0;
  bool aligned = false;
  bool have_index = false;
  bool previous_index = false;
  disk_err_t result = DISK_ERR_TIMEOUT;

  while (transitions < FLOPPY_READ_TRANSITIONS_MAX &&
         completed_revolutions < FLOPPY_READ_REVOLUTIONS) {
    if ((transitions & 0xFFu) == 0 && deadline_elapsed(deadline)) break;
    floppy_pulse_t pulse;
    status = floppy_flux_next_internal(f, deadline, expected_generation, &pulse);
    if (status != DISK_OK) {
      result = status;
      break;
    }
    transitions++;
    bool boundary = have_index && previous_index && !pulse.index;
    previous_index = pulse.index;
    have_index = true;
    if (boundary) {
      if (aligned) {
        completed_revolutions++;
        if (goal->conflict != 0) {
          result = goal->expected ? DISK_ERR_VERIFY : DISK_ERR_CORRUPT;
          break;
        }
        if (goal->verify_failed) {
          result = DISK_ERR_VERIFY;
          break;
        }
        if (read_goal_complete(goal)) {
          clean++;
          if (clean >= clean_revolutions) {
            result = DISK_OK;
            break;
          }
        } else {
          clean = 0;
        }
      }
      aligned = true;
      mfm_reset(&decoder);
      goal->seen = 0;
      continue;
    }
    if (!aligned) continue;
    if (mfm_feed(&decoder, pulse.delta, &sector)) read_goal_accept(goal, &sector);
  }

  if (result == DISK_ERR_TIMEOUT) result = read_goal_verdict(goal, &decoder);
  floppy_flux_read_stop(f);
  floppy_update_media(f);
  if (result == DISK_OK && f->media_generation != expected_generation) {
    result = DISK_ERR_MEDIA_CHANGED;
  }
  return result;
}

static disk_err_t floppy_read_recover(floppy_t *f, uint32_t expected_generation,
                                      read_goal_t *goal, unsigned clean_revolutions,
                                      const floppy_deadline_t *deadline) {
  disk_err_t status = floppy_read_flux(f, expected_generation, goal,
                                       clean_revolutions, deadline);
  if (status == DISK_OK || !floppy_status_retryable(status)) return status;
  if (status == DISK_ERR_WRONG_TRACK) {
    f->stats.retries++;
    f->track0_confirmed = false;
    status = floppy_read_flux(f, expected_generation, goal, clean_revolutions,
                              deadline);
    if (status == DISK_OK) {
      f->stats.recovered++;
      return DISK_OK;
    }
    if (!floppy_status_retryable(status)) return status;
  }
  static const uint8_t distances[] = {10, 20};
  for (size_t i = 0; i < sizeof(distances); i++) {
    if (deadline_elapsed(deadline)) return DISK_ERR_TIMEOUT;
    f->stats.retries++;
    status = floppy_jog(f, goal->cylinder, distances[i], deadline,
                        expected_generation);
    if (status != DISK_OK) return status;
    status = floppy_read_flux(f, expected_generation, goal, clean_revolutions,
                              deadline);
    if (status == DISK_OK) {
      f->stats.recovered++;
      return DISK_OK;
    }
    if (!floppy_status_retryable(status)) return status;
  }
  return status;
}

disk_err_t floppy_read_track(floppy_t *f, uint32_t expected_generation,
                             track_t *track) {
  if (!floppy_ready(f) || !track || !disk_ch_valid(track->cylinder, track->head)) {
    return DISK_ERR_INVALID;
  }
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_READ, FLOPPY_READ_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  track->valid = 0;
  f->stats.reads++;
  read_goal_t goal = {
      .track = track,
      .cylinder = track->cylinder,
      .head = track->head,
  };
  status = floppy_read_recover(f, expected_generation, &goal, 1, &deadline);
  if (status != DISK_OK) floppy_stats_failure(f, status);
  floppy_operation_end(f, FLOPPY_OPERATION_READ);
  return status;
}

static void floppy_flux_write_prepare(floppy_t *f) {
  pio_sm_set_enabled(f->write.pio, f->write.sm, false);
  dma_channel_abort(floppy_dma_channel(f));
  pio_sm_clear_fifos(f->write.pio, f->write.sm);
  pio_sm_restart(f->write.pio, f->write.sm);
  pio_sm_exec(f->write.pio, f->write.sm, pio_encode_jmp(f->write.offset));
  pio_clear_stall(f->write.pio, pio_tx_stall_mask(f));
  pio_sm_set_pins_with_mask(f->write.pio, f->write.sm,
                            1u << f->pins.write_data, 1u << f->pins.write_data);
  floppy_pin_oc(f->pins.write_gate, true);
}

static void floppy_flux_write_open(floppy_t *f) {
  floppy_pin_oc(f->pins.write_gate, false);
  pio_sm_set_enabled(f->write.pio, f->write.sm, true);
}

static void floppy_flux_write_close(floppy_t *f) {
  floppy_pin_oc(f->pins.write_gate, true);
  pio_sm_set_enabled(f->write.pio, f->write.sm, false);
  dma_channel_abort(floppy_dma_channel(f));
  pio_sm_set_pins_with_mask(f->write.pio, f->write.sm,
                            1u << f->pins.write_data, 1u << f->pins.write_data);
}

typedef struct {
  floppy_t *floppy;
  const floppy_deadline_t *deadline;
  disk_err_t status;
  uint32_t generation;
  uint32_t start_ms;
  uint32_t produced;
  uint32_t completed;
  uint32_t canonical;
  uint16_t fill;
  uint16_t dma_len;
  uint8_t fill_half;
  uint8_t poll_count;
  bool dma_active;
  bool started;
  bool index_armed;
  bool boundary;
  bool canonical_done;
} flux_tx_t;

static bool flux_tx_fail(flux_tx_t *tx, disk_err_t status) {
  if (tx->status == DISK_OK) tx->status = status;
  floppy_flux_write_close(tx->floppy);
  return false;
}

static bool flux_tx_underrun(flux_tx_t *tx) {
  if (tx->status == DISK_OK) {
    tx->status = DISK_ERR_UNDERRUN;
    tx->floppy->stats.underruns++;
  }
  if (tx->started) floppy_flux_write_close(tx->floppy);
  return false;
}

static uint32_t flux_tx_fed(flux_tx_t *tx) {
  uint32_t fed = tx->completed;
  if (tx->dma_active) {
    uint32_t left =
        dma_channel_hw_addr(floppy_dma_channel(tx->floppy))->transfer_count;
    if (left <= tx->dma_len) fed += tx->dma_len - left;
  }
  return fed;
}

static bool flux_tx_poll(flux_tx_t *tx) {
  if (tx->status != DISK_OK || tx->boundary) return false;
  floppy_t *f = tx->floppy;
  if (floppy_flux_media_status(f, tx->generation) != DISK_OK) {
    return flux_tx_fail(tx, DISK_ERR_MEDIA_CHANGED);
  }
  if (floppy_write_protected_raw(f)) return flux_tx_fail(tx, DISK_ERR_WRITE_PROTECTED);
  if (tx->started && pio_stalled(f->write.pio, pio_tx_stall_mask(f))) {
    return flux_tx_underrun(tx);
  }
  if (deadline_elapsed(tx->deadline) ||
      (tx->started && elapsed(tx->start_ms, FLOPPY_WRITE_FLUX_TIMEOUT_MS))) {
    return flux_tx_fail(tx, DISK_ERR_TIMEOUT);
  }
  if (!tx->started) return true;
  bool index = gpio_get(f->pins.index);
  if (index) tx->index_armed = true;
  if (!tx->index_armed || index) return true;
  uint32_t fed = flux_tx_fed(tx);
  tx->boundary = true;
  if (!tx->canonical_done || fed < tx->canonical + FLOPPY_TX_FIFO_DEPTH) {
    tx->status = DISK_ERR_VERIFY;
  }
  floppy_flux_write_close(f);
  return false;
}

static void flux_tx_configure(flux_tx_t *tx, uint8_t half, uint16_t length) {
  floppy_t *f = tx->floppy;
  dma_channel_config config = dma_channel_get_default_config(floppy_dma_channel(f));
  channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
  channel_config_set_read_increment(&config, true);
  channel_config_set_write_increment(&config, false);
  channel_config_set_dreq(&config, pio_get_dreq(f->write.pio, f->write.sm, true));
  channel_config_set_high_priority(&config, true);
  const uint8_t *source = (const uint8_t *)f->flux_ring + half * FLOPPY_TX_HALF_BYTES;
  dma_channel_configure(floppy_dma_channel(f), &config,
                        &f->write.pio->txf[f->write.sm], source,
                        length, true);
  tx->dma_len = length;
  tx->dma_active = true;
}

static bool flux_tx_preload(flux_tx_t *tx) {
  uint32_t target = tx->dma_len > FLOPPY_TX_FIFO_DEPTH
                        ? tx->dma_len - FLOPPY_TX_FIFO_DEPTH
                        : 0;
  uint32_t start = now_ms();
  while (dma_channel_hw_addr(floppy_dma_channel(tx->floppy))->transfer_count > target) {
    if (floppy_flux_media_status(tx->floppy, tx->generation) != DISK_OK) {
      return flux_tx_fail(tx, DISK_ERR_MEDIA_CHANGED);
    }
    if (floppy_write_protected_raw(tx->floppy)) {
      return flux_tx_fail(tx, DISK_ERR_WRITE_PROTECTED);
    }
    if (elapsed(start, FLOPPY_INDEX_TIMEOUT_MS) || deadline_elapsed(tx->deadline)) {
      return flux_tx_fail(tx, DISK_ERR_TIMEOUT);
    }
    tight_loop_contents();
  }
  return true;
}

static bool flux_tx_wait_dma(flux_tx_t *tx) {
  while (dma_channel_is_busy(floppy_dma_channel(tx->floppy))) {
    if (!flux_tx_poll(tx)) return false;
    tight_loop_contents();
  }
  tx->completed += tx->dma_len;
  tx->dma_active = false;
  if (pio_stalled(tx->floppy->write.pio, pio_tx_stall_mask(tx->floppy))) {
    return flux_tx_underrun(tx);
  }
  return true;
}

static bool flux_tx_submit(flux_tx_t *tx) {
  if (tx->fill == 0 || tx->status != DISK_OK || tx->boundary) return false;
  uint8_t half = tx->fill_half;
  uint16_t length = tx->fill;
  if (!tx->started) {
    floppy_flux_write_prepare(tx->floppy);
    flux_tx_configure(tx, half, length);
    if (!flux_tx_preload(tx)) return false;
    disk_err_t status = floppy_wait_index_falling(
        tx->floppy, tx->deadline, FLOPPY_INDEX_TIMEOUT_MS, tx->generation);
    if (status != DISK_OK) return flux_tx_fail(tx, status);
    if (floppy_write_protected_raw(tx->floppy)) {
      return flux_tx_fail(tx, DISK_ERR_WRITE_PROTECTED);
    }
    if (floppy_flux_media_status(tx->floppy, tx->generation) != DISK_OK) {
      return flux_tx_fail(tx, DISK_ERR_MEDIA_CHANGED);
    }
    tx->started = true;
    tx->start_ms = now_ms();
    tx->index_armed = false;
    floppy_flux_write_open(tx->floppy);
  } else {
    if (!flux_tx_wait_dma(tx)) return false;
    flux_tx_configure(tx, half, length);
  }
  tx->fill_half ^= 1u;
  tx->fill = 0;
  return true;
}

static bool flux_tx_emit(void *ctx, uint8_t pulse) {
  flux_tx_t *tx = ctx;
  if (tx->status != DISK_OK || tx->boundary) return false;
  tx->poll_count++;
  if (tx->poll_count == FLOPPY_TX_POLL_INTERVAL) {
    tx->poll_count = 0;
    if (!flux_tx_poll(tx)) return false;
  }
  if (tx->produced >= FLOPPY_WRITE_TRANSITIONS_MAX) {
    tx->status = DISK_ERR_INVALID;
    if (tx->started) floppy_flux_write_close(tx->floppy);
    return false;
  }
  uint8_t *buffer = (uint8_t *)tx->floppy->flux_ring +
                    tx->fill_half * FLOPPY_TX_HALF_BYTES;
  buffer[tx->fill++] = pulse;
  tx->produced++;
  if (tx->fill == FLOPPY_TX_HALF_BYTES && !flux_tx_submit(tx)) return false;
  return true;
}

static disk_err_t floppy_write_flux(floppy_t *f, uint32_t expected_generation,
                                    const track_t *track,
                                    const floppy_deadline_t *deadline) {
  flux_tx_t tx = {
      .floppy = f,
      .deadline = deadline,
      .status = DISK_OK,
      .generation = expected_generation,
  };
  mfm_encode_t encoder;
  mfm_encode_init_emit(&encoder, flux_tx_emit, &tx);
  mfm_encode_track(&encoder, track);
  if (tx.status != DISK_OK) return tx.status;
  if (encoder.stopped) return DISK_ERR_INVALID;
  tx.canonical = tx.produced;
  tx.canonical_done = true;
  while (tx.status == DISK_OK && !tx.boundary) {
    mfm_encode_gap(&encoder, 32);
  }
  if (tx.status != DISK_OK) return tx.status;
  if (!tx.boundary) {
    floppy_flux_write_close(f);
    return DISK_ERR_TIMEOUT;
  }
  f->stats.dma_writes++;
  return DISK_OK;
}

static disk_err_t floppy_write_track_owned(floppy_t *f,
                                           uint32_t expected_generation,
                                           const track_t *track,
                                           const floppy_deadline_t *deadline) {
  floppy_update_media(f);
  if (f->media_generation != expected_generation) return DISK_ERR_MEDIA_CHANGED;
  if (floppy_write_protected_raw(f)) return DISK_ERR_WRITE_PROTECTED;
  disk_err_t status = floppy_prepare(f, expected_generation, deadline);
  if (status != DISK_OK) return status;
  if (f->media_generation != expected_generation) return DISK_ERR_MEDIA_CHANGED;
  disk_err_t last = DISK_ERR_VERIFY;
  for (unsigned attempt = 0; attempt < FLOPPY_WRITE_ATTEMPTS; attempt++) {
    if (deadline_elapsed(deadline)) return DISK_ERR_TIMEOUT;
    floppy_update_media(f);
    if (f->media_generation != expected_generation || f->disk_change_active) {
      return DISK_ERR_MEDIA_CHANGED;
    }
    if (floppy_write_protected_raw(f)) return DISK_ERR_WRITE_PROTECTED;
    if (attempt == 2u) f->track0_confirmed = false;
    status = floppy_seek_internal(f, track->cylinder, deadline, expected_generation);
    if (status != DISK_OK) return status;
    status = floppy_side_select_internal(f, track->head, deadline);
    if (status != DISK_OK) return status;
    status = floppy_write_flux(f, expected_generation, track, deadline);
    if (status != DISK_OK) {
      last = status;
      if (!floppy_status_retryable(status)) break;
      f->stats.retries++;
      continue;
    }
    read_goal_t goal = {
        .expected = track,
        .cylinder = track->cylinder,
        .head = track->head,
    };
    status = floppy_read_flux(f, expected_generation, &goal, 2, deadline);
    floppy_update_media(f);
    if (f->media_generation != expected_generation) return DISK_ERR_MEDIA_CHANGED;
    if (status == DISK_OK) return DISK_OK;
    last = status == DISK_ERR_TIMEOUT || status == DISK_ERR_CRC ||
                   status == DISK_ERR_CORRUPT
               ? DISK_ERR_VERIFY
               : status;
    if (!floppy_status_retryable(status)) break;
    f->stats.retries++;
    status = floppy_jog(f, track->cylinder, 10, deadline, expected_generation);
    if (status != DISK_OK) {
      last = status;
      break;
    }
  }
  return last;
}

disk_err_t floppy_write_track(floppy_t *f, uint32_t expected_generation,
                              const track_t *track) {
  if (!floppy_ready(f) || !track || !disk_ch_valid(track->cylinder, track->head) ||
      track->valid != DISK_TRACK_VALID) {
    return DISK_ERR_INVALID;
  }
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_WRITE, FLOPPY_WRITE_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  status = floppy_write_track_owned(f, expected_generation, track, &deadline);
  if (status != DISK_OK) floppy_stats_failure(f, status);
  floppy_operation_end(f, FLOPPY_OPERATION_WRITE);
  return status;
}

static void floppy_release(floppy_t *f) {
  if (f->gpio_configured) {
    floppy_pin_oc(f->pins.write_gate, true);
    floppy_stop(f);
  }
  if (f->read_sm_claimed) pio_sm_set_enabled(f->read.pio, f->read.sm, false);
  if (f->write_sm_claimed) {
    pio_sm_set_enabled(f->write.pio, f->write.sm, false);
    pio_sm_set_pins_with_mask(f->write.pio, f->write.sm,
                              1u << f->pins.write_data,
                              1u << f->pins.write_data);
  }
  if (f->dma_claimed) {
    dma_channel_abort(floppy_dma_channel(f));
    dma_channel_unclaim(floppy_dma_channel(f));
    f->dma_claimed = false;
    f->dma_ch = -1;
  }
  if (f->gpio_configured) {
    uint8_t pins[] = FLOPPY_PIN_LIST(f->pins);
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
      gpio_deinit(pins[i]);
    }
    f->gpio_configured = false;
  }
  if (f->read_sm_claimed) {
    pio_sm_unclaim(f->read.pio, f->read.sm);
    f->read_sm_claimed = false;
  }
  if (f->write_sm_claimed) {
    pio_sm_unclaim(f->write.pio, f->write.sm);
    f->write_sm_claimed = false;
  }
  if (f->read_program_loaded) {
    pio_remove_program(f->read.pio, &flux_read_program, f->read.offset);
    f->read_program_loaded = false;
  }
  if (f->write_program_loaded) {
    pio_remove_program(f->write.pio, &flux_write_program, f->write.offset);
    f->write_program_loaded = false;
  }
  f->flux_active = false;
  f->operation = FLOPPY_OPERATION_IDLE;
  f->lifecycle = 0;
  if (active_floppy == f) active_floppy = NULL;
}

disk_err_t floppy_deinit(floppy_t *f) {
  floppy_deadline_t deadline;
  disk_err_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_TEARDOWN, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != DISK_OK) return status;
  floppy_release(f);
  return DISK_OK;
}

disk_err_t floppy_init(floppy_t *f, floppy_pins_t pins) {
  if (!f) return DISK_ERR_INVALID;
  if ((uintptr_t)f % _Alignof(floppy_t) != 0) return DISK_ERR_INVALID;
  if (active_floppy) return DISK_ERR_BUSY;
  uint32_t system_clock = clock_get_hz(clk_sys);
  if (system_clock == 0 || system_clock % MFM_READ_PIO_HZ != 0 ||
      !floppy_pins_valid(&pins)) {
    return DISK_ERR_INVALID;
  }
  memset(f, 0, sizeof(*f));
  f->pins = pins;
  f->dma_ch = -1;
  f->read.pio = pio0;
  f->write.pio = pio1;
  if (pio_get_gpio_base(f->read.pio) != 0 || pio_get_gpio_base(f->write.pio) != 0) {
    return DISK_ERR_INVALID;
  }
  active_floppy = f;

  if (!pio_can_add_program(f->read.pio, &flux_read_program) ||
      !pio_can_add_program(f->write.pio, &flux_write_program)) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  int offset = pio_add_program(f->read.pio, &flux_read_program);
  if (offset < 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  f->read.offset = (uint)offset;
  f->read_program_loaded = true;
  int sm = pio_claim_unused_sm(f->read.pio, false);
  if (sm < 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  f->read.sm = (uint)sm;
  f->read_sm_claimed = true;

  offset = pio_add_program(f->write.pio, &flux_write_program);
  if (offset < 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  f->write.offset = (uint)offset;
  f->write_program_loaded = true;
  sm = pio_claim_unused_sm(f->write.pio, false);
  if (sm < 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  f->write.sm = (uint)sm;
  f->write_sm_claimed = true;

  f->dma_ch = dma_claim_unused_channel(false);
  if (f->dma_ch < 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  f->dma_claimed = true;

  uint inputs[] = {f->pins.index, f->pins.track0, f->pins.write_protect,
                   f->pins.read_data, f->pins.disk_change};
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
    gpio_init(inputs[i]);
    gpio_set_dir(inputs[i], GPIO_IN);
    gpio_pull_up(inputs[i]);
  }
  uint outputs[] = {f->pins.drive_select, f->pins.motor_enable, f->pins.direction,
                    f->pins.step, f->pins.write_data, f->pins.write_gate,
                    f->pins.side_select, f->pins.density};
  for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
    gpio_init(outputs[i]);
    gpio_disable_pulls(outputs[i]);
    gpio_put(outputs[i], 0);
    gpio_set_dir(outputs[i], GPIO_IN);
  }
  f->gpio_configured = true;
  floppy_pin_oc(f->pins.density, false);

  if (flux_read_program_init(f->read.pio, f->read.sm, f->read.offset,
                             f->pins.read_data, f->pins.index) != 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  pio_sm_clear_fifos(f->read.pio, f->read.sm);
  pio_sm_restart(f->read.pio, f->read.sm);
  pio_sm_set_enabled(f->read.pio, f->read.sm, false);
  if (flux_write_program_init(f->write.pio, f->write.sm, f->write.offset,
                              f->pins.write_data) != 0) {
    floppy_release(f);
    return DISK_ERR_IO;
  }
  pio_sm_set_enabled(f->write.pio, f->write.sm, false);

  f->operation = FLOPPY_OPERATION_IDLE;
  f->last_io_time_ms = now_ms();
  f->lifecycle = FLOPPY_LIFECYCLE;
  return DISK_OK;
}

static disk_err_t floppy_io_read_track(void *ctx, uint32_t expected_generation,
                                       uint8_t cylinder, uint8_t head,
                                       track_t *track) {
  if (!track) return DISK_ERR_INVALID;
  track->cylinder = cylinder;
  track->head = head;
  return floppy_read_track(ctx, expected_generation, track);
}

static disk_err_t floppy_io_write_track(void *ctx, uint32_t expected_generation,
                                        const track_t *track) {
  return floppy_write_track(ctx, expected_generation, track);
}

static disk_err_t floppy_io_media_generation(void *ctx, uint32_t *generation) {
  return floppy_media_generation(ctx, generation);
}

static disk_err_t floppy_io_write_protected(void *ctx, bool *write_protected) {
  return floppy_write_protected(ctx, write_protected);
}

disk_device_t floppy_device(floppy_t *f) {
  return (disk_device_t){
      .read_track = floppy_io_read_track,
      .write_track = floppy_io_write_track,
      .media_generation = floppy_io_media_generation,
      .write_protected = floppy_io_write_protected,
      .ctx = f,
  };
}
