#include "floppy.h"
#include "flux_read.pio.h"
#include "flux_write.pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "mfm_decode.h"
#include "mfm_encode.h"
#include "pico/stdlib.h"
#include <string.h>

#define DIR_INWARD 1
#define DIR_OUTWARD 2
#define IDLE_CHECK_INTERVAL_MS 1000
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

static uint32_t floppy_lock(floppy_t *f) {
  return spin_lock_blocking(spin_lock_instance(f->lock_num));
}

static void floppy_unlock(floppy_t *f, uint32_t state) {
  spin_unlock(spin_lock_instance(f->lock_num), state);
}

static block_status_t floppy_operation_begin(floppy_t *f, floppy_operation_t operation,
                                             uint32_t limit_ms,
                                             floppy_deadline_t *deadline) {
  if (!floppy_ready(f) || !f->lock_claimed || !deadline) return BLOCK_ERR_INVALID;
  uint32_t lock_state = floppy_lock(f);
  if (f->lifecycle != FLOPPY_LIFECYCLE || f->operation != FLOPPY_OPERATION_IDLE) {
    floppy_unlock(f, lock_state);
    return BLOCK_ERR_BUSY;
  }
  *deadline = deadline_begin(limit_ms);
  f->operation = operation;
  f->operation_start_ms = deadline->start_ms;
  f->operation_limit_ms = deadline->limit_ms;
  floppy_unlock(f, lock_state);
  return BLOCK_OK;
}

static void floppy_operation_end(floppy_t *f, floppy_operation_t operation) {
  uint32_t lock_state = floppy_lock(f);
  if (f->operation == operation) {
    f->operation = FLOPPY_OPERATION_IDLE;
    f->operation_start_ms = 0;
    f->operation_limit_ms = 0;
  }
  floppy_unlock(f, lock_state);
}

static bool floppy_pins_valid(const floppy_pins_t *pins) {
  const uint8_t values[] = {
      pins->index,         pins->track0,       pins->write_protect,
      pins->read_data,     pins->disk_change,  pins->drive_select,
      pins->motor_enable,  pins->direction,    pins->step,
      pins->write_data,    pins->write_gate,   pins->side_select,
      pins->density,
  };
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
  bool active = !gpio_get(f->pins.disk_change);
  uint32_t lock_state = floppy_lock(f);
  if (active && !f->disk_change_active) {
    f->disk_change_active = true;
    f->track0_confirmed = false;
    f->motor_qualified = false;
    f->media_generation = next_generation(f->media_generation);
    f->stats.media_changes++;
  } else if (!active) {
    f->disk_change_active = false;
  }
  floppy_unlock(f, lock_state);
}

static uint32_t floppy_generation_value(floppy_t *f) {
  uint32_t lock_state = floppy_lock(f);
  uint32_t generation = f->media_generation;
  floppy_unlock(f, lock_state);
  return generation;
}

static bool floppy_change_active(floppy_t *f) {
  uint32_t lock_state = floppy_lock(f);
  bool active = f->disk_change_active;
  floppy_unlock(f, lock_state);
  return active;
}

static block_status_t floppy_flux_media_status(floppy_t *f,
                                               uint32_t generation) {
  if (gpio_get(f->pins.disk_change)) return BLOCK_OK;
  floppy_update_media(f);
  return floppy_generation_value(f) == generation && !floppy_change_active(f)
             ? BLOCK_OK
             : BLOCK_ERR_MEDIA_CHANGED;
}

static void floppy_track_invalidate(floppy_t *f) {
  uint32_t lock_state = floppy_lock(f);
  f->track0_confirmed = false;
  floppy_unlock(f, lock_state);
}

static bool floppy_track_confirmed(floppy_t *f) {
  uint32_t lock_state = floppy_lock(f);
  bool confirmed = f->track0_confirmed;
  floppy_unlock(f, lock_state);
  return confirmed;
}

static uint8_t floppy_cylinder_value(floppy_t *f) {
  uint32_t lock_state = floppy_lock(f);
  uint8_t cylinder = f->cylinder;
  floppy_unlock(f, lock_state);
  return cylinder;
}

static bool floppy_idle_timer_callback(struct repeating_timer *timer) {
  floppy_t *f = timer->user_data;
  if (!floppy_ready(f) || !f->lock_claimed) return false;
  uint32_t lock_state = floppy_lock(f);
  uint32_t now = now_ms();
  if (!f->motor_on || f->operation != FLOPPY_OPERATION_IDLE ||
      (uint32_t)(now - f->last_io_time_ms) < FLOPPY_IDLE_TIMEOUT_MS) {
    floppy_unlock(f, lock_state);
    return true;
  }
  floppy_pin_oc(f->pins.motor_enable, true);
  f->motor_on = false;
  f->motor_qualified = false;
  floppy_pin_oc(f->pins.drive_select, true);
  f->selected = false;
  floppy_unlock(f, lock_state);
  return true;
}

static void floppy_touch(floppy_t *f) {
  uint32_t lock_state = floppy_lock(f);
  f->last_io_time_ms = now_ms();
  floppy_unlock(f, lock_state);
}

static block_status_t floppy_wait_index_falling(floppy_t *f,
                                                 const floppy_deadline_t *deadline,
                                                 uint32_t timeout,
                                                 uint32_t generation) {
  uint32_t start = now_ms();
  while (!gpio_get(f->pins.index)) {
    floppy_update_media(f);
    if (floppy_generation_value(f) != generation) return BLOCK_ERR_MEDIA_CHANGED;
    if (elapsed(start, timeout) || deadline_elapsed(deadline)) return BLOCK_ERR_TIMEOUT;
    tight_loop_contents();
  }
  while (gpio_get(f->pins.index)) {
    floppy_update_media(f);
    if (floppy_generation_value(f) != generation) return BLOCK_ERR_MEDIA_CHANGED;
    if (elapsed(start, timeout) || deadline_elapsed(deadline)) return BLOCK_ERR_TIMEOUT;
    tight_loop_contents();
  }
  return BLOCK_OK;
}

static block_status_t floppy_wait_stable_index(floppy_t *f,
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
    block_status_t status = floppy_wait_index_falling(f, deadline, left, generation);
    if (status != BLOCK_OK) return status;
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
      if (stable >= 2u) return BLOCK_OK;
    }
    previous = now;
  }
  return BLOCK_ERR_TIMEOUT;
}

static block_status_t floppy_select_internal(floppy_t *f, bool on,
                                              const floppy_deadline_t *deadline) {
  if (f->selected == on) return BLOCK_OK;
  floppy_pin_oc(f->pins.drive_select, !on);
  f->selected = on;
  sleep_ms(10);
  floppy_touch(f);
  return deadline_elapsed(deadline) ? BLOCK_ERR_TIMEOUT : BLOCK_OK;
}

static void floppy_motor_line_on(floppy_t *f) {
  if (!f->motor_on) {
    floppy_pin_oc(f->pins.motor_enable, false);
    f->motor_on = true;
    f->motor_qualified = false;
  }
  floppy_touch(f);
}

static block_status_t floppy_motor_off_internal(floppy_t *f) {
  if (!f->motor_on) return BLOCK_OK;
  floppy_pin_oc(f->pins.motor_enable, true);
  f->motor_on = false;
  f->motor_qualified = false;
  floppy_touch(f);
  return BLOCK_OK;
}

static block_status_t floppy_qualify_motor(floppy_t *f,
                                           const floppy_deadline_t *deadline,
                                           uint32_t generation) {
  if (f->motor_qualified && f->motor_generation == generation) return BLOCK_OK;
  block_status_t status = floppy_wait_stable_index(f, deadline, generation);
  if (status != BLOCK_OK) return status;
  floppy_update_media(f);
  if (floppy_generation_value(f) != generation) return BLOCK_ERR_MEDIA_CHANGED;
  if (floppy_change_active(f)) return BLOCK_ERR_MEDIA_CHANGED;
  f->motor_generation = generation;
  f->motor_qualified = true;
  return BLOCK_OK;
}

static block_status_t floppy_clear_media_latch(floppy_t *f,
                                                const floppy_deadline_t *deadline,
                                                uint32_t generation);

static block_status_t floppy_prepare(floppy_t *f, uint32_t expected_generation,
                                     const floppy_deadline_t *deadline) {
  floppy_update_media(f);
  if (floppy_generation_value(f) != expected_generation) {
    return BLOCK_ERR_MEDIA_CHANGED;
  }
  floppy_touch(f);
  block_status_t status = floppy_select_internal(f, true, deadline);
  if (status != BLOCK_OK) return status;
  floppy_motor_line_on(f);
  status = floppy_clear_media_latch(f, deadline, expected_generation);
  if (status != BLOCK_OK) return status;
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

static block_status_t flux_read_wait(floppy_t *f, const floppy_deadline_t *deadline,
                                     uint32_t generation, uint16_t *out) {
  if (!flux_data_available(f)) {
    uint32_t start = now_ms();
    while (!flux_data_available(f)) {
      block_status_t status = floppy_flux_media_status(f, generation);
      if (status != BLOCK_OK) return status;
      if (pio_stalled(f->read.pio, pio_rx_stall_mask(f))) {
        f->stats.overruns++;
        return BLOCK_ERR_OVERRUN;
      }
      if (f->read_overrun) return BLOCK_ERR_OVERRUN;
      if (elapsed(start, FLOPPY_FLUX_WAIT_TIMEOUT_MS) || deadline_elapsed(deadline)) {
        return BLOCK_ERR_TIMEOUT;
      }
      tight_loop_contents();
    }
  }
  *out = flux_read_sample(f);
  if (pio_stalled(f->read.pio, pio_rx_stall_mask(f))) {
    f->stats.overruns++;
    return BLOCK_ERR_OVERRUN;
  }
  block_status_t status = floppy_flux_media_status(f, generation);
  if (status != BLOCK_OK) return status;
  return f->read_overrun ? BLOCK_ERR_OVERRUN : BLOCK_OK;
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

static block_status_t floppy_flux_session_start(floppy_t *f,
                                                uint32_t expected_generation) {
  if (f->flux_active) return BLOCK_ERR_INVALID;
  floppy_update_media(f);
  if (floppy_generation_value(f) != expected_generation || floppy_change_active(f)) {
    return BLOCK_ERR_MEDIA_CHANGED;
  }
  floppy_flux_read_start(f);
  f->flux_generation = expected_generation;
  f->flux_active = true;
  return BLOCK_OK;
}

block_status_t floppy_flux_begin(floppy_t *f, uint32_t expected_generation) {
  floppy_deadline_t deadline;
  block_status_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_RAW, FLOPPY_READ_DEADLINE_MS, &deadline);
  if (status != BLOCK_OK) return status;
  status = floppy_prepare(f, expected_generation, &deadline);
  if (status == BLOCK_OK) status = floppy_flux_session_start(f, expected_generation);
  if (status != BLOCK_OK) floppy_operation_end(f, FLOPPY_OPERATION_RAW);
  return status;
}

static block_status_t floppy_flux_next_internal(floppy_t *f,
                                                const floppy_deadline_t *deadline,
                                                uint32_t generation, uint16_t *delta,
                                                bool *index) {
  uint16_t value;
  if (!f->read.primed) {
    block_status_t status = flux_read_wait(f, deadline, generation, &value);
    if (status != BLOCK_OK) return status;
    f->read.prev = value >> 1;
    f->read.primed = true;
  }
  block_status_t status = flux_read_wait(f, deadline, generation, &value);
  if (status != BLOCK_OK) return status;
  uint16_t count = value >> 1;
  int difference = f->read.prev - count;
  if (difference < 0) difference += 0x8000;
  f->read.prev = count;
  *delta = (uint16_t)difference;
  *index = (value & 1u) != 0;
  return BLOCK_OK;
}

block_status_t floppy_flux_next(floppy_t *f, uint16_t *delta, bool *index) {
  if (!floppy_ready(f) || f->operation != FLOPPY_OPERATION_RAW || !f->flux_active ||
      !delta || !index) {
    return BLOCK_ERR_INVALID;
  }
  floppy_deadline_t deadline = {
      .start_ms = f->operation_start_ms,
      .limit_ms = f->operation_limit_ms,
  };
  return floppy_flux_next_internal(f, &deadline, f->flux_generation, delta, index);
}

block_status_t floppy_flux_end(floppy_t *f) {
  if (!floppy_ready(f) || f->operation != FLOPPY_OPERATION_RAW || !f->flux_active) {
    return BLOCK_ERR_INVALID;
  }
  floppy_flux_read_stop(f);
  floppy_operation_end(f, FLOPPY_OPERATION_RAW);
  return BLOCK_OK;
}

static block_status_t floppy_side_select_internal(floppy_t *f, uint8_t head,
                                                   const floppy_deadline_t *deadline) {
  if (head >= DISK_HEADS) return BLOCK_ERR_INVALID;
  bool changed = !f->head_confirmed || f->head != head;
  floppy_pin_oc(f->pins.side_select, head == 0);
  f->head = head;
  f->head_confirmed = true;
  if (changed) sleep_ms(FLOPPY_HEAD_SETTLE_MS);
  floppy_touch(f);
  return deadline_elapsed(deadline) ? BLOCK_ERR_TIMEOUT : BLOCK_OK;
}

static block_status_t floppy_step_internal(floppy_t *f, int direction,
                                            const floppy_deadline_t *deadline,
                                            uint32_t generation) {
  if (deadline_elapsed(deadline)) return BLOCK_ERR_TIMEOUT;
  floppy_pin_oc(f->pins.direction, direction != DIR_INWARD);
  sleep_us(10);
  floppy_pin_oc(f->pins.step, false);
  sleep_us(10);
  floppy_pin_oc(f->pins.step, true);
  sleep_ms(FLOPPY_STEP_MS);
  uint32_t lock_state = floppy_lock(f);
  if (direction == DIR_INWARD && f->cylinder + 1u < DISK_CYLINDERS) {
    f->cylinder++;
  } else if (direction == DIR_OUTWARD && f->cylinder > 0) {
    f->cylinder--;
  }
  floppy_unlock(f, lock_state);
  floppy_update_media(f);
  if (floppy_generation_value(f) != generation) return BLOCK_ERR_MEDIA_CHANGED;
  return deadline_elapsed(deadline) ? BLOCK_ERR_TIMEOUT : BLOCK_OK;
}

static block_status_t floppy_clear_media_latch(floppy_t *f,
                                                const floppy_deadline_t *deadline,
                                                uint32_t generation) {
  floppy_update_media(f);
  if (!floppy_change_active(f)) return BLOCK_OK;
  block_status_t status = floppy_step_internal(f, DIR_OUTWARD, deadline, generation);
  if (status != BLOCK_OK) return status;
  sleep_ms(FLOPPY_HEAD_SETTLE_MS);
  if (deadline_elapsed(deadline)) return BLOCK_ERR_TIMEOUT;
  floppy_update_media(f);
  if (floppy_generation_value(f) != generation || floppy_change_active(f)) {
    return BLOCK_ERR_MEDIA_CHANGED;
  }
  floppy_track_invalidate(f);
  return BLOCK_OK;
}

static block_status_t floppy_seek_track0_internal(floppy_t *f,
                                                   const floppy_deadline_t *deadline,
                                                   uint32_t generation, bool *moved) {
  floppy_track_invalidate(f);
  for (unsigned step = 0; step < 90u; step++) {
    if (!gpio_get(f->pins.track0)) {
      uint32_t lock_state = floppy_lock(f);
      f->cylinder = 0;
      f->track0_confirmed = true;
      floppy_unlock(f, lock_state);
      return BLOCK_OK;
    }
    block_status_t status = floppy_step_internal(f, DIR_OUTWARD, deadline, generation);
    if (status != BLOCK_OK) return status;
    *moved = true;
  }
  return BLOCK_ERR_NO_TRACK0;
}

static block_status_t floppy_seek_internal(floppy_t *f, uint8_t target,
                                            const floppy_deadline_t *deadline,
                                            uint32_t generation) {
  if (target >= DISK_CYLINDERS) return BLOCK_ERR_WRONG_TRACK;
  bool moved = false;
  if (!floppy_track_confirmed(f)) {
    block_status_t status = floppy_seek_track0_internal(f, deadline, generation, &moved);
    if (status != BLOCK_OK) return status;
  }
  while (floppy_cylinder_value(f) < target) {
    block_status_t status = floppy_step_internal(f, DIR_INWARD, deadline, generation);
    if (status != BLOCK_OK) return status;
    moved = true;
  }
  while (floppy_cylinder_value(f) > target) {
    block_status_t status = floppy_step_internal(f, DIR_OUTWARD, deadline, generation);
    if (status != BLOCK_OK) return status;
    moved = true;
  }
  if (moved) sleep_ms(FLOPPY_HEAD_SETTLE_MS);
  floppy_touch(f);
  return deadline_elapsed(deadline) ? BLOCK_ERR_TIMEOUT : BLOCK_OK;
}

block_status_t floppy_select(floppy_t *f, bool on) {
  floppy_deadline_t deadline;
  block_status_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != BLOCK_OK) return status;
  status = floppy_select_internal(f, on, &deadline);
  floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}

block_status_t floppy_motor_on(floppy_t *f) {
  floppy_deadline_t deadline;
  block_status_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != BLOCK_OK) return status;
  floppy_update_media(f);
  uint32_t generation = floppy_generation_value(f);
  status = floppy_select_internal(f, true, &deadline);
  if (status == BLOCK_OK) {
    floppy_motor_line_on(f);
    status = floppy_wait_stable_index(f, &deadline, generation);
    if (status == BLOCK_OK && !floppy_change_active(f)) {
      f->motor_generation = generation;
      f->motor_qualified = true;
    }
  }
  if (status != BLOCK_OK) floppy_motor_off_internal(f);
  floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}

block_status_t floppy_motor_off(floppy_t *f) {
  floppy_deadline_t deadline;
  block_status_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != BLOCK_OK) return status;
  status = floppy_motor_off_internal(f);
  floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}

block_status_t floppy_side_select(floppy_t *f, uint8_t head) {
  if (head >= DISK_HEADS) return BLOCK_ERR_INVALID;
  floppy_deadline_t deadline;
  block_status_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != BLOCK_OK) return status;
  status = floppy_side_select_internal(f, head, &deadline);
  floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}

block_status_t floppy_seek(floppy_t *f, uint8_t target) {
  if (target >= DISK_CYLINDERS) return BLOCK_ERR_WRONG_TRACK;
  floppy_deadline_t deadline;
  block_status_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_CONTROL, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != BLOCK_OK) return status;
  floppy_update_media(f);
  uint32_t generation = floppy_generation_value(f);
  status = floppy_prepare(f, generation, &deadline);
  if (status == BLOCK_OK) {
    status = floppy_seek_internal(f, target, &deadline, generation);
  }
  floppy_operation_end(f, FLOPPY_OPERATION_CONTROL);
  return status;
}

block_status_t floppy_current_track(const floppy_t *f, uint8_t *cylinder) {
  if (!floppy_ready(f) || !cylinder) return BLOCK_ERR_INVALID;
  floppy_t *mutable = (floppy_t *)f;
  uint32_t lock_state = floppy_lock(mutable);
  if (!f->track0_confirmed) {
    floppy_unlock(mutable, lock_state);
    return BLOCK_ERR_NO_TRACK0;
  }
  *cylinder = f->cylinder;
  floppy_unlock(mutable, lock_state);
  return BLOCK_OK;
}

block_status_t floppy_at_track0(const floppy_t *f, bool *active) {
  if (!floppy_ready(f) || !active) return BLOCK_ERR_INVALID;
  *active = !gpio_get(f->pins.track0);
  return BLOCK_OK;
}

block_status_t floppy_disk_changed(floppy_t *f, bool *changed) {
  if (!floppy_ready(f) || !changed) return BLOCK_ERR_INVALID;
  floppy_update_media(f);
  *changed = floppy_change_active(f);
  return BLOCK_OK;
}

block_status_t floppy_media_generation(floppy_t *f, uint32_t *generation) {
  if (!floppy_ready(f) || !generation) return BLOCK_ERR_INVALID;
  floppy_update_media(f);
  *generation = floppy_generation_value(f);
  return BLOCK_OK;
}

block_status_t floppy_write_protected(const floppy_t *f, bool *write_protected) {
  if (!floppy_ready(f) || !write_protected) return BLOCK_ERR_INVALID;
  *write_protected = !gpio_get(f->pins.write_protect);
  return BLOCK_OK;
}

static bool floppy_write_protected_raw(const floppy_t *f) {
  return !gpio_get(f->pins.write_protect);
}

block_status_t floppy_stats_reset(floppy_t *f) {
  if (!floppy_ready(f)) return BLOCK_ERR_INVALID;
  uint32_t lock_state = floppy_lock(f);
  if (f->operation != FLOPPY_OPERATION_IDLE) {
    floppy_unlock(f, lock_state);
    return BLOCK_ERR_BUSY;
  }
  memset(&f->stats, 0, sizeof(f->stats));
  floppy_unlock(f, lock_state);
  return BLOCK_OK;
}

block_status_t floppy_stats(const floppy_t *f, floppy_stats_t *stats) {
  if (!floppy_ready(f) || !stats) return BLOCK_ERR_INVALID;
  floppy_t *mutable = (floppy_t *)f;
  uint32_t lock_state = floppy_lock(mutable);
  if (f->operation != FLOPPY_OPERATION_IDLE) {
    floppy_unlock(mutable, lock_state);
    return BLOCK_ERR_BUSY;
  }
  *stats = f->stats;
  floppy_unlock(mutable, lock_state);
  return BLOCK_OK;
}

static bool floppy_status_retryable(block_status_t status) {
  return status == BLOCK_ERR_TIMEOUT || status == BLOCK_ERR_CRC ||
         status == BLOCK_ERR_OVERRUN || status == BLOCK_ERR_UNDERRUN ||
         status == BLOCK_ERR_VERIFY || status == BLOCK_ERR_WRONG_TRACK;
}

static void floppy_stats_failure(floppy_t *f, block_status_t status) {
  if (status == BLOCK_OK) return;
  f->stats.failed++;
  if (status == BLOCK_ERR_TIMEOUT) f->stats.timeout++;
  else if (status == BLOCK_ERR_CRC) f->stats.crc++;
  else if (status == BLOCK_ERR_WRONG_TRACK) f->stats.wrong_track++;
  else if (status == BLOCK_ERR_WRONG_SIDE) f->stats.wrong_side++;
}

static block_status_t floppy_jog(floppy_t *f, uint8_t cylinder, uint8_t distance,
                                  const floppy_deadline_t *deadline,
                                  uint32_t generation) {
  floppy_touch(f);
  uint8_t away = cylinder <= distance ? cylinder + distance : cylinder - distance;
  block_status_t status = floppy_seek_internal(f, away, deadline, generation);
  if (status != BLOCK_OK) return status;
  return floppy_seek_internal(f, cylinder, deadline, generation);
}

typedef enum {
  READ_SECTOR,
  READ_TRACK,
  READ_VERIFY,
} read_kind_t;

typedef struct {
  read_kind_t kind;
  uint8_t cylinder;
  uint8_t head;
  uint8_t sector;
  uint8_t *data;
  track_t *track;
  const track_t *expected;
  uint32_t seen;
  uint32_t conflict;
  bool candidate;
  bool verify_failed;
  bool correct_ch;
  bool wrong_track;
  bool wrong_side;
} read_goal_t;

static void read_goal_reset(read_goal_t *goal) {
  goal->seen = 0;
  goal->candidate = false;
}

static block_status_t read_goal_accept(read_goal_t *goal, const mfm_sector_t *sector) {
  if (!goal || !sector ||
      (goal->kind == READ_SECTOR && !goal->data) ||
      (goal->kind == READ_TRACK && !goal->track) ||
      (goal->kind == READ_VERIFY && !goal->expected) ||
      (goal->kind != READ_SECTOR && goal->kind != READ_TRACK &&
       goal->kind != READ_VERIFY)) {
    return BLOCK_ERR_INVALID;
  }
  if (sector->cylinder != goal->cylinder) {
    goal->wrong_track = true;
    return BLOCK_OK;
  }
  if (sector->head != goal->head) {
    goal->wrong_side = true;
    return BLOCK_OK;
  }
  goal->correct_ch = true;
  if (!disk_sector_valid(sector->sector)) return BLOCK_ERR_CORRUPT;
  uint32_t bit = 1u << sector->sector;
  if (goal->seen & bit) {
    if (goal->kind == READ_TRACK ||
        (goal->kind == READ_SECTOR && sector->sector == goal->sector)) {
      goal->conflict |= bit;
      if (goal->kind == READ_TRACK) goal->track->valid &= ~bit;
    }
    if (goal->kind == READ_VERIFY) {
      goal->conflict |= bit;
      goal->verify_failed = true;
    }
    return BLOCK_OK;
  }
  goal->seen |= bit;
  if (goal->kind == READ_SECTOR) {
    if (sector->sector != goal->sector) return BLOCK_OK;
    memcpy(goal->data, sector->data, DISK_SECTOR_SIZE);
    goal->candidate = true;
    return BLOCK_OK;
  }
  if (goal->kind == READ_TRACK) {
    if (goal->conflict & bit) return BLOCK_OK;
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
  } else if (memcmp(goal->expected->data[sector->sector], sector->data,
                    DISK_SECTOR_SIZE) != 0) {
    goal->verify_failed = true;
  }
  return BLOCK_OK;
}

static block_status_t floppy_read_flux(floppy_t *f, uint32_t expected_generation,
                                       read_goal_t *goal, unsigned clean_revolutions,
                                       const floppy_deadline_t *deadline) {
  floppy_update_media(f);
  if (floppy_generation_value(f) != expected_generation) return BLOCK_ERR_MEDIA_CHANGED;
  block_status_t status = floppy_prepare(f, expected_generation, deadline);
  if (status != BLOCK_OK) return status;
  if (floppy_generation_value(f) != expected_generation) return BLOCK_ERR_MEDIA_CHANGED;
  status = floppy_seek_internal(f, goal->cylinder, deadline, expected_generation);
  if (status != BLOCK_OK) return status;
  status = floppy_side_select_internal(f, goal->head, deadline);
  if (status != BLOCK_OK) return status;
  status = floppy_flux_session_start(f, expected_generation);
  if (status != BLOCK_OK) return status;

  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  read_goal_reset(goal);
  uint32_t transitions = 0;
  uint32_t completed_revolutions = 0;
  unsigned clean = 0;
  bool aligned = false;
  bool have_index = false;
  bool previous_index = false;
  block_status_t result = BLOCK_ERR_TIMEOUT;

  while (transitions < FLOPPY_READ_TRANSITIONS_MAX &&
         completed_revolutions < FLOPPY_READ_REVOLUTIONS) {
    if ((transitions & 0xFFu) == 0 && deadline_elapsed(deadline)) break;
    uint16_t delta;
    bool index;
    status = floppy_flux_next_internal(f, deadline, expected_generation, &delta, &index);
    if (status != BLOCK_OK) {
      result = status;
      break;
    }
    transitions++;
    bool boundary = have_index && previous_index && !index;
    previous_index = index;
    have_index = true;
    if (boundary) {
      if (aligned) {
        completed_revolutions++;
        if (goal->kind == READ_SECTOR) {
          uint32_t requested = 1u << goal->sector;
          if (goal->conflict & requested) {
            result = BLOCK_ERR_CORRUPT;
            break;
          }
          if (goal->candidate) {
            result = BLOCK_OK;
            break;
          }
        } else if (goal->conflict != 0) {
          result = goal->kind == READ_VERIFY ? BLOCK_ERR_VERIFY : BLOCK_ERR_CORRUPT;
          break;
        } else if (goal->kind == READ_VERIFY && goal->verify_failed) {
          result = BLOCK_ERR_VERIFY;
          break;
        } else {
          bool pass = goal->kind == READ_TRACK
                          ? goal->track->valid == DISK_TRACK_VALID
                          : goal->seen == DISK_TRACK_VALID;
          if (pass) {
          clean++;
          if (clean >= clean_revolutions) {
            result = BLOCK_OK;
            break;
          }
          } else {
            clean = 0;
          }
        }
      }
      aligned = true;
      mfm_reset(&decoder);
      read_goal_reset(goal);
      continue;
    }
    if (!aligned) continue;
    if (mfm_feed(&decoder, delta, &sector)) {
      status = read_goal_accept(goal, &sector);
      if (status != BLOCK_OK) {
        result = status;
        break;
      }
    }
  }

  if (result == BLOCK_ERR_TIMEOUT && goal->conflict) {
    result = goal->kind == READ_VERIFY ? BLOCK_ERR_VERIFY : BLOCK_ERR_CORRUPT;
  }
  else if (result == BLOCK_ERR_TIMEOUT && goal->verify_failed) result = BLOCK_ERR_VERIFY;
  else if (result == BLOCK_ERR_TIMEOUT && decoder.crc_errors > 0) result = BLOCK_ERR_CRC;
  else if (result == BLOCK_ERR_TIMEOUT && decoder.format_errors > 0) {
    result = BLOCK_ERR_CORRUPT;
  }
  else if (result == BLOCK_ERR_TIMEOUT && goal->kind == READ_VERIFY &&
           goal->correct_ch) result = BLOCK_ERR_VERIFY;
  else if (result == BLOCK_ERR_TIMEOUT && !goal->correct_ch && goal->wrong_track) {
    result = BLOCK_ERR_WRONG_TRACK;
  } else if (result == BLOCK_ERR_TIMEOUT && !goal->correct_ch && goal->wrong_side) {
    result = BLOCK_ERR_WRONG_SIDE;
  }
  floppy_flux_read_stop(f);
  floppy_update_media(f);
  if (result == BLOCK_OK && floppy_generation_value(f) != expected_generation) {
    result = BLOCK_ERR_MEDIA_CHANGED;
  }
  return result;
}

static block_status_t floppy_read_recover(floppy_t *f, uint32_t expected_generation,
                                          read_goal_t *goal,
                                          unsigned clean_revolutions,
                                          const floppy_deadline_t *deadline) {
  block_status_t status = floppy_read_flux(f, expected_generation, goal,
                                           clean_revolutions, deadline);
  if (status == BLOCK_OK || !floppy_status_retryable(status)) return status;
  if (status == BLOCK_ERR_WRONG_TRACK) {
    f->stats.retries++;
    floppy_track_invalidate(f);
    status = floppy_read_flux(f, expected_generation, goal, clean_revolutions,
                              deadline);
    if (status == BLOCK_OK) {
      f->stats.recovered++;
      return BLOCK_OK;
    }
    if (!floppy_status_retryable(status)) return status;
  }
  static const uint8_t distances[] = {10, 20};
  for (size_t i = 0; i < sizeof(distances); i++) {
    if (deadline_elapsed(deadline)) return BLOCK_ERR_TIMEOUT;
    f->stats.retries++;
    status = floppy_jog(f, goal->cylinder, distances[i], deadline,
                        expected_generation);
    if (status != BLOCK_OK) return status;
    status = floppy_read_flux(f, expected_generation, goal, clean_revolutions,
                              deadline);
    if (status == BLOCK_OK) {
      f->stats.recovered++;
      return BLOCK_OK;
    }
    if (!floppy_status_retryable(status)) return status;
  }
  return status;
}

block_status_t floppy_read_sector(floppy_t *f, uint32_t expected_generation,
                                  uint8_t cylinder, uint8_t head, uint8_t sector,
                                  uint8_t out[DISK_SECTOR_SIZE]) {
  if (!floppy_ready(f) || !disk_ch_valid(cylinder, head) ||
      !disk_sector_valid(sector) || !out) {
    return BLOCK_ERR_INVALID;
  }
  floppy_deadline_t deadline;
  block_status_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_READ, FLOPPY_READ_DEADLINE_MS, &deadline);
  if (status != BLOCK_OK) return status;
  f->stats.reads++;
  read_goal_t goal = {
      .kind = READ_SECTOR,
      .cylinder = cylinder,
      .head = head,
      .sector = sector,
      .data = out,
  };
  status = floppy_read_recover(f, expected_generation, &goal, 1, &deadline);
  if (status != BLOCK_OK) floppy_stats_failure(f, status);
  floppy_operation_end(f, FLOPPY_OPERATION_READ);
  return status;
}

block_status_t floppy_read_track(floppy_t *f, uint32_t expected_generation,
                                 track_t *track) {
  if (!floppy_ready(f) || !track || !disk_ch_valid(track->cylinder, track->head)) {
    return BLOCK_ERR_INVALID;
  }
  floppy_deadline_t deadline;
  block_status_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_READ, FLOPPY_READ_DEADLINE_MS, &deadline);
  if (status != BLOCK_OK) return status;
  track->valid = 0;
  f->stats.reads++;
  read_goal_t goal = {
      .kind = READ_TRACK,
      .cylinder = track->cylinder,
      .head = track->head,
      .track = track,
  };
  status = floppy_read_recover(f, expected_generation, &goal, 1, &deadline);
  if (status != BLOCK_OK) floppy_stats_failure(f, status);
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
  block_status_t status;
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

static bool flux_tx_underrun(flux_tx_t *tx) {
  if (tx->status == BLOCK_OK) {
    tx->status = BLOCK_ERR_UNDERRUN;
    tx->floppy->stats.underruns++;
  }
  if (tx->started) floppy_flux_write_close(tx->floppy);
  return false;
}

static uint32_t flux_tx_fed(flux_tx_t *tx) {
  uint32_t fed = tx->completed;
  if (tx->dma_active) {
    uint32_t remaining =
        dma_channel_hw_addr(floppy_dma_channel(tx->floppy))->transfer_count;
    if (remaining <= tx->dma_len) fed += tx->dma_len - remaining;
  }
  return fed;
}

static bool flux_tx_poll(flux_tx_t *tx) {
  if (tx->status != BLOCK_OK || tx->boundary) return false;
  floppy_t *f = tx->floppy;
  if (floppy_flux_media_status(f, tx->generation) != BLOCK_OK) {
    tx->status = BLOCK_ERR_MEDIA_CHANGED;
    floppy_flux_write_close(f);
    return false;
  }
  if (floppy_write_protected_raw(f)) {
    tx->status = BLOCK_ERR_WRITE_PROTECTED;
    floppy_flux_write_close(f);
    return false;
  }
  if (tx->started && pio_stalled(f->write.pio, pio_tx_stall_mask(f))) {
    return flux_tx_underrun(tx);
  }
  if (deadline_elapsed(tx->deadline) ||
      (tx->started && elapsed(tx->start_ms, FLOPPY_WRITE_FLUX_TIMEOUT_MS))) {
    tx->status = BLOCK_ERR_TIMEOUT;
    floppy_flux_write_close(f);
    return false;
  }
  if (!tx->started) return true;
  bool index = gpio_get(f->pins.index);
  if (index) tx->index_armed = true;
  if (!tx->index_armed || index) return true;
  uint32_t fed = flux_tx_fed(tx);
  tx->boundary = true;
  if (!tx->canonical_done || fed < tx->canonical + FLOPPY_TX_FIFO_DEPTH) {
    tx->status = BLOCK_ERR_VERIFY;
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
    if (floppy_flux_media_status(tx->floppy, tx->generation) != BLOCK_OK) {
      tx->status = BLOCK_ERR_MEDIA_CHANGED;
      return false;
    }
    if (floppy_write_protected_raw(tx->floppy)) {
      tx->status = BLOCK_ERR_WRITE_PROTECTED;
      return false;
    }
    if (elapsed(start, FLOPPY_INDEX_TIMEOUT_MS) || deadline_elapsed(tx->deadline)) {
      tx->status = BLOCK_ERR_TIMEOUT;
      return false;
    }
    tight_loop_contents();
  }
  return true;
}

static bool flux_tx_wait_dma(flux_tx_t *tx) {
  while (dma_channel_is_busy(floppy_dma_channel(tx->floppy))) {
    dma_channel_hw_addr(floppy_dma_channel(tx->floppy));
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
  if (tx->fill == 0 || tx->status != BLOCK_OK || tx->boundary) return false;
  uint8_t half = tx->fill_half;
  uint16_t length = tx->fill;
  if (!tx->started) {
    floppy_flux_write_prepare(tx->floppy);
    flux_tx_configure(tx, half, length);
    if (!flux_tx_preload(tx)) {
      floppy_flux_write_close(tx->floppy);
      return false;
    }
    block_status_t status = floppy_wait_index_falling(
        tx->floppy, tx->deadline, FLOPPY_INDEX_TIMEOUT_MS, tx->generation);
    if (status != BLOCK_OK) {
      tx->status = status;
      floppy_flux_write_close(tx->floppy);
      return false;
    }
    if (floppy_write_protected_raw(tx->floppy)) {
      tx->status = BLOCK_ERR_WRITE_PROTECTED;
      floppy_flux_write_close(tx->floppy);
      return false;
    }
    if (floppy_flux_media_status(tx->floppy, tx->generation) != BLOCK_OK) {
      tx->status = BLOCK_ERR_MEDIA_CHANGED;
      floppy_flux_write_close(tx->floppy);
      return false;
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
  if (tx->status != BLOCK_OK || tx->boundary) return false;
  tx->poll_count++;
  if (tx->poll_count == FLOPPY_TX_POLL_INTERVAL) {
    tx->poll_count = 0;
    if (!flux_tx_poll(tx)) return false;
  }
  if (tx->produced >= FLOPPY_WRITE_TRANSITIONS_MAX) {
    tx->status = BLOCK_ERR_INVALID;
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

static block_status_t floppy_write_flux(floppy_t *f, uint32_t expected_generation,
                                        const track_t *track,
                                        const floppy_deadline_t *deadline) {
  flux_tx_t tx = {
      .floppy = f,
      .deadline = deadline,
      .status = BLOCK_OK,
      .generation = expected_generation,
  };
  mfm_encode_t encoder;
  mfm_encode_init_emit(&encoder, flux_tx_emit, &tx);
  mfm_encode_track(&encoder, track);
  if (tx.status != BLOCK_OK) return tx.status;
  if (encoder.stopped) return BLOCK_ERR_INVALID;
  tx.canonical = tx.produced;
  tx.canonical_done = true;
  while (tx.status == BLOCK_OK && !tx.boundary) {
    mfm_encode_gap(&encoder, 32);
  }
  if (tx.status != BLOCK_OK) return tx.status;
  if (!tx.boundary) {
    floppy_flux_write_close(f);
    return BLOCK_ERR_TIMEOUT;
  }
  f->stats.dma_writes++;
  return BLOCK_OK;
}

static block_status_t floppy_write_track_owned(floppy_t *f,
                                                uint32_t expected_generation,
                                                const track_t *track,
                                                const floppy_deadline_t *deadline) {
  floppy_update_media(f);
  if (floppy_generation_value(f) != expected_generation) return BLOCK_ERR_MEDIA_CHANGED;
  if (floppy_write_protected_raw(f)) return BLOCK_ERR_WRITE_PROTECTED;
  block_status_t status = floppy_prepare(f, expected_generation, deadline);
  if (status != BLOCK_OK) return status;
  if (floppy_generation_value(f) != expected_generation) return BLOCK_ERR_MEDIA_CHANGED;
  block_status_t last = BLOCK_ERR_VERIFY;
  for (unsigned attempt = 0; attempt < FLOPPY_WRITE_ATTEMPTS; attempt++) {
    if (deadline_elapsed(deadline)) return BLOCK_ERR_TIMEOUT;
    floppy_update_media(f);
    if (floppy_generation_value(f) != expected_generation || floppy_change_active(f)) {
      return BLOCK_ERR_MEDIA_CHANGED;
    }
    if (floppy_write_protected_raw(f)) return BLOCK_ERR_WRITE_PROTECTED;
    if (attempt == 2u) floppy_track_invalidate(f);
    status = floppy_seek_internal(f, track->cylinder, deadline, expected_generation);
    if (status != BLOCK_OK) return status;
    status = floppy_side_select_internal(f, track->head, deadline);
    if (status != BLOCK_OK) return status;
    status = floppy_write_flux(f, expected_generation, track, deadline);
    if (status != BLOCK_OK) {
      last = status;
      if (!floppy_status_retryable(status)) break;
      f->stats.retries++;
      continue;
    }
    read_goal_t goal = {
        .kind = READ_VERIFY,
        .cylinder = track->cylinder,
        .head = track->head,
        .expected = track,
    };
    status = floppy_read_flux(f, expected_generation, &goal, 2, deadline);
    floppy_update_media(f);
    if (floppy_generation_value(f) != expected_generation) {
      return BLOCK_ERR_MEDIA_CHANGED;
    }
    if (status == BLOCK_OK) return BLOCK_OK;
    last = status == BLOCK_ERR_TIMEOUT || status == BLOCK_ERR_CRC ||
                   status == BLOCK_ERR_CORRUPT
               ? BLOCK_ERR_VERIFY
               : status;
    if (!floppy_status_retryable(status)) break;
    f->stats.retries++;
    status = floppy_jog(f, track->cylinder, 10, deadline, expected_generation);
    if (status != BLOCK_OK) {
      last = status;
      break;
    }
  }
  return last;
}

block_status_t floppy_write_track(floppy_t *f, uint32_t expected_generation,
                                  const track_t *track) {
  if (!floppy_ready(f) || !track || !disk_ch_valid(track->cylinder, track->head) ||
      track->valid != DISK_TRACK_VALID) {
    return BLOCK_ERR_INVALID;
  }
  floppy_deadline_t deadline;
  block_status_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_WRITE, FLOPPY_WRITE_DEADLINE_MS, &deadline);
  if (status != BLOCK_OK) return status;
  status = floppy_write_track_owned(f, expected_generation, track, &deadline);
  if (status != BLOCK_OK) floppy_stats_failure(f, status);
  floppy_operation_end(f, FLOPPY_OPERATION_WRITE);
  return status;
}

static block_status_t floppy_release(floppy_t *f) {
  block_status_t status = BLOCK_OK;
  if (f->gpio_configured) {
    floppy_pin_oc(f->pins.write_gate, true);
    floppy_pin_oc(f->pins.motor_enable, true);
    floppy_pin_oc(f->pins.drive_select, true);
  }
  if (f->timer_active) {
    if (!cancel_repeating_timer(&f->idle_timer)) status = BLOCK_ERR_IO;
    f->timer_active = false;
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
    uint pins[] = {
        f->pins.index,         f->pins.track0,       f->pins.write_protect,
        f->pins.read_data,     f->pins.disk_change,  f->pins.drive_select,
        f->pins.motor_enable,  f->pins.direction,    f->pins.step,
        f->pins.write_data,    f->pins.write_gate,   f->pins.side_select,
        f->pins.density,
    };
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
  f->motor_on = false;
  f->motor_qualified = false;
  f->selected = false;
  f->flux_active = false;
  f->operation = FLOPPY_OPERATION_IDLE;
  f->lifecycle = 0;
  if (f->lock_claimed) {
    spin_lock_unclaim(f->lock_num);
    f->lock_claimed = false;
  }
  if (active_floppy == f) active_floppy = NULL;
  return status;
}

block_status_t floppy_deinit(floppy_t *f) {
  floppy_deadline_t deadline;
  block_status_t status = floppy_operation_begin(
      f, FLOPPY_OPERATION_TEARDOWN, FLOPPY_CONTROL_DEADLINE_MS, &deadline);
  if (status != BLOCK_OK) return status;
  return floppy_release(f);
}

block_status_t floppy_init(floppy_t *f, floppy_pins_t pins) {
  if (!f) return BLOCK_ERR_INVALID;
  if ((uintptr_t)f % _Alignof(floppy_t) != 0) return BLOCK_ERR_INVALID;
  if (active_floppy) return BLOCK_ERR_BUSY;
  uint32_t system_clock = clock_get_hz(clk_sys);
  if (system_clock == 0 || system_clock % 72000000u != 0 ||
      !floppy_pins_valid(&pins)) {
    return BLOCK_ERR_INVALID;
  }
  memset(f, 0, sizeof(*f));
  f->pins = pins;
  f->dma_ch = -1;
  f->read.pio = pio0;
  f->write.pio = pio1;
  if (pio_get_gpio_base(f->read.pio) != 0 || pio_get_gpio_base(f->write.pio) != 0) {
    return BLOCK_ERR_INVALID;
  }

  int lock_num = spin_lock_claim_unused(false);
  if (lock_num < 0) return BLOCK_ERR_IO;
  f->lock_num = (uint8_t)lock_num;
  f->lock_claimed = true;
  active_floppy = f;

  if (!pio_can_add_program(f->read.pio, &flux_read_program) ||
      !pio_can_add_program(f->write.pio, &flux_write_program)) {
    floppy_release(f);
    return BLOCK_ERR_IO;
  }
  int offset = pio_add_program(f->read.pio, &flux_read_program);
  if (offset < 0) {
    floppy_release(f);
    return BLOCK_ERR_IO;
  }
  f->read.offset = (uint)offset;
  f->read_program_loaded = true;
  int sm = pio_claim_unused_sm(f->read.pio, false);
  if (sm < 0) {
    floppy_release(f);
    return BLOCK_ERR_IO;
  }
  f->read.sm = (uint)sm;
  f->read_sm_claimed = true;

  offset = pio_add_program(f->write.pio, &flux_write_program);
  if (offset < 0) {
    floppy_release(f);
    return BLOCK_ERR_IO;
  }
  f->write.offset = (uint)offset;
  f->write_program_loaded = true;
  sm = pio_claim_unused_sm(f->write.pio, false);
  if (sm < 0) {
    floppy_release(f);
    return BLOCK_ERR_IO;
  }
  f->write.sm = (uint)sm;
  f->write_sm_claimed = true;

  f->dma_ch = dma_claim_unused_channel(false);
  if (f->dma_ch < 0) {
    floppy_release(f);
    return BLOCK_ERR_IO;
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
    gpio_put(outputs[i], 0);
    gpio_set_dir(outputs[i], GPIO_IN);
  }
  f->gpio_configured = true;
  floppy_pin_oc(f->pins.density, false);

  if (flux_read_program_init(f->read.pio, f->read.sm, f->read.offset,
                             f->pins.read_data, f->pins.index) != 0) {
    floppy_release(f);
    return BLOCK_ERR_IO;
  }
  pio_sm_clear_fifos(f->read.pio, f->read.sm);
  pio_sm_restart(f->read.pio, f->read.sm);
  pio_sm_set_enabled(f->read.pio, f->read.sm, false);
  if (flux_write_program_init(f->write.pio, f->write.sm, f->write.offset,
                              f->pins.write_data) != 0) {
    floppy_release(f);
    return BLOCK_ERR_IO;
  }
  pio_sm_set_enabled(f->write.pio, f->write.sm, false);

  f->operation = FLOPPY_OPERATION_IDLE;
  f->last_io_time_ms = now_ms();
  f->lifecycle = FLOPPY_LIFECYCLE;
  if (!add_repeating_timer_ms(IDLE_CHECK_INTERVAL_MS, floppy_idle_timer_callback, f,
                              &f->idle_timer)) {
    floppy_release(f);
    return BLOCK_ERR_IO;
  }
  f->timer_active = true;
  return BLOCK_OK;
}

static block_status_t floppy_io_read_track(void *ctx,
                                           uint32_t expected_generation,
                                           uint8_t cylinder, uint8_t head,
                                           track_t *track) {
  if (!track) return BLOCK_ERR_INVALID;
  track->cylinder = cylinder;
  track->head = head;
  return floppy_read_track(ctx, expected_generation, track);
}

static block_status_t floppy_io_write_track(void *ctx,
                                            uint32_t expected_generation,
                                            const track_t *track) {
  return floppy_write_track(ctx, expected_generation, track);
}

static block_status_t floppy_io_media_generation(void *ctx,
                                                 uint32_t *generation) {
  return floppy_media_generation(ctx, generation);
}

static block_status_t floppy_io_write_protected(void *ctx,
                                                bool *write_protected) {
  return floppy_write_protected(ctx, write_protected);
}

block_device_t floppy_device(floppy_t *f) {
  return (block_device_t){
      .read_track = floppy_io_read_track,
      .write_track = floppy_io_write_track,
      .media_generation = floppy_io_media_generation,
      .write_protected = floppy_io_write_protected,
      .ctx = f,
  };
}
