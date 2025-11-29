#include "floppy.h"
#include "mfm_decode.h"
#include "mfm_encode.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "flux_read.pio.h"
#include "flux_write.pio.h"

// Debug levels: 0=none, 1=errors, 2=warnings, 3=verbose
#define FLOPPY_DEBUG 1

#if FLOPPY_DEBUG >= 1
#define FLOPPY_ERR(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define FLOPPY_ERR(fmt, ...)
#endif

#if FLOPPY_DEBUG >= 2
#define FLOPPY_WARN(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define FLOPPY_WARN(fmt, ...)
#endif

#if FLOPPY_DEBUG >= 3
#define FLOPPY_DBG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define FLOPPY_DBG(fmt, ...)
#endif

// ============== Constants ==============
#define DIR_INWARD 1
#define DIR_OUTWARD 2
#define IDLE_CHECK_INTERVAL_MS 1000  // Check idle every 1 second

// ============== Internal Helpers ==============

static void gpio_put_oc(uint pin, bool value);  // Forward declare

// ============== Idle Timer Callback ==============

static bool floppy_idle_timer_callback(struct repeating_timer *t) {
  floppy_t *f = (floppy_t *)t->user_data;

  // Skip if motor already off
  if (!f->motor_on) {
    return true;
  }

  // Check if idle timeout has elapsed
  uint32_t now = to_ms_since_boot(get_absolute_time());
  if (now - f->last_io_time_ms >= FLOPPY_IDLE_TIMEOUT_MS) {
    gpio_put_oc(f->pins.motor_enable, 1);  // Motor off
    f->motor_on = false;
    gpio_put_oc(f->pins.drive_select, 1);  // Deselect
    f->selected = false;
  }

  return true;
}

// ============== Auto Motor Helper ==============

// Called before any operation that needs the drive
static void floppy_prepare(floppy_t *f) {
  if (!f->auto_motor) return;

  f->last_io_time_ms = to_ms_since_boot(get_absolute_time());
  floppy_select(f, true);
  floppy_motor_on(f);
}

// ============== GPIO Helpers ==============

static void gpio_put_oc(uint pin, bool value) {
  if (value == 0) {
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_OUT);  // Drive low
  } else {
    gpio_set_dir(pin, GPIO_IN);   // High-Z (pulled high by drive)
  }
}

static inline bool flux_data_available(floppy_t *f) {
  return f->read.half || !pio_sm_is_rx_fifo_empty(f->read.pio, f->read.sm);
}

static inline uint16_t flux_read(floppy_t *f) {
  if (f->read.half) {
    uint16_t v = f->read.half;
    f->read.half = 0;
    return v;
  }
  uint32_t pv = pio_sm_get_blocking(f->read.pio, f->read.sm);
  f->read.half = pv >> 16;
  return pv & 0xffff;
}

static inline uint16_t flux_read_wait(floppy_t *f) {
  while (!flux_data_available(f)) {
    tight_loop_contents();
  }
  return flux_read(f);
}

static void floppy_flux_read_start(floppy_t *f) {
  pio_sm_exec(f->read.pio, f->read.sm, pio_encode_jmp(f->read.offset));
  pio_sm_restart(f->read.pio, f->read.sm);
  pio_sm_clear_fifos(f->read.pio, f->read.sm);
  pio_sm_set_enabled(f->read.pio, f->read.sm, true);
}

static void floppy_flux_read_stop(floppy_t *f) {
  pio_sm_set_enabled(f->read.pio, f->read.sm, false);
}

static void floppy_flux_write_start(floppy_t *f) {
  pio_sm_set_enabled(f->write.pio, f->write.sm, false);
  pio_sm_clear_fifos(f->write.pio, f->write.sm);
  pio_sm_restart(f->write.pio, f->write.sm);
  pio_sm_exec(f->write.pio, f->write.sm, pio_encode_jmp(f->write.offset));

  gpio_put_oc(f->pins.write_gate, 0);

  pio_sm_set_enabled(f->write.pio, f->write.sm, true);
}

static void floppy_flux_write_stop(floppy_t *f) {
  // Drain FIFO
  while (!pio_sm_is_tx_fifo_empty(f->write.pio, f->write.sm)) {
    tight_loop_contents();
  }
  // Wait for last pulse
  sleep_us(100);
  gpio_put_oc(f->pins.write_gate, 1);
  pio_sm_set_enabled(f->write.pio, f->write.sm, false);
}

static void floppy_wait_for_index(floppy_t *f) {
  while (!gpio_get(f->pins.index)) tight_loop_contents();  // Wait for high (not at index)
  while (gpio_get(f->pins.index)) tight_loop_contents();   // Wait for falling edge (index starts)
}

static void floppy_side_select(floppy_t *f, uint8_t side) {
  gpio_put_oc(f->pins.side_select, side == 0 ? 1 : 0);  // Side 0 = High, Side 1 = Low
}

static void floppy_step(floppy_t *f, int direction) {
  gpio_put_oc(f->pins.direction, direction == DIR_INWARD ? 0 : 1);

  sleep_us(10);
  gpio_put_oc(f->pins.step, 0);
  sleep_us(10);
  gpio_put_oc(f->pins.step, 1);

  sleep_ms(10);

  if (direction == DIR_INWARD && f->track < FLOPPY_TRACKS - 1) {
    f->track++;
  } else if (direction == DIR_OUTWARD && f->track > 0) {
    f->track--;
  }
}

static floppy_status_t floppy_seek_track0(floppy_t *f) {
  f->track0_confirmed = false;
  for (int i = 0; i < 90; i++) {
    if (floppy_at_track0(f)) {
      f->track = 0;
      f->track0_confirmed = true;
      return FLOPPY_OK;
    }
    floppy_step(f, DIR_OUTWARD);
  }
  return FLOPPY_ERR_NO_TRACK0;
}
#define FLOPPY_READ_TRACK_ATTEMPTS 20
// Read track, only filling sectors that are not already valid
static floppy_status_t floppy_complete_track(floppy_t *f, track_t *t) {
  // Check if track is already complete - skip read entirely
  bool complete = true;
  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    if (!t->sectors[i].valid) {
      complete = false;
      break;
    }
  }
  if (complete) {
    return FLOPPY_OK;
  }

  floppy_seek(f, t->track);
  floppy_side_select(f, t->side);
  floppy_flux_read_start(f);

  mfm_t mfm;
  sector_t sector = {0};
  mfm_init(&mfm);
  mfm_reset(&mfm);

  uint16_t prev = flux_read_wait(f) >> 1;
  bool ix_prev = false;

  floppy_status_t res = FLOPPY_ERR_TIMEOUT;
  for (int ix_edges = 0; ix_edges < FLOPPY_READ_TRACK_ATTEMPTS * 2;) {
    uint16_t value = flux_read_wait(f);
    uint8_t ix = value & 1;
    uint16_t cnt = value >> 1;
    int delta = prev - cnt;
    if (delta < 0) delta += 0x8000;

    if (ix != ix_prev) ix_edges++;  // Index pin status changed
    ix_prev = ix;

    if (mfm_feed(&mfm, delta, &sector)) {
      if (sector.valid && sector.sector_n >= 1 && sector.sector_n <= SECTORS_PER_TRACK) {
        if (sector.track != t->track) {
          FLOPPY_ERR("[floppy] wrong track: expected %d, got %d\n", t->track, sector.track);
          res = FLOPPY_ERR_WRONG_TRACK;
          break;
        }

        if (sector.side != t->side) {
          FLOPPY_ERR("[floppy] wrong side: expected %d, got %d\n", t->side, sector.side);
          res = FLOPPY_ERR_WRONG_SIDE;
          break;
        }

        // Only update missing sectors
        if (!t->sectors[sector.sector_n - 1].valid) {
          t->sectors[sector.sector_n - 1] = sector;
        }

        // Check if we have all sectors
        bool complete = true;
        for (int i = 0; i < SECTORS_PER_TRACK; i++) {
          if (!t->sectors[i].valid) {
            complete = false;
            break;
          }
        }
        if (complete) {
          res = FLOPPY_OK;
          break;
        }
      }
    }

    prev = cnt;
  }

  floppy_flux_read_stop(f);

  if (res == FLOPPY_ERR_TIMEOUT) {
    FLOPPY_ERR("[floppy] timeout reading track %d side %d, missing sectors:", t->track, t->side);
    for (int i = 0; i < SECTORS_PER_TRACK; i++) {
      if (!t->sectors[i].valid) FLOPPY_ERR(" %d", i + 1);
    }
    FLOPPY_ERR("\n");
  }

  return res;
}

// Internal read that returns status
static floppy_status_t floppy_read_internal(floppy_t *f, int track, int side, int sector_n, sector_t *out) {
  floppy_seek(f, track);
  floppy_side_select(f, side);

  mfm_t mfm;
  sector_t sector = {0};
  mfm_init(&mfm);
  mfm_reset(&mfm);

  floppy_flux_read_start(f);

  uint16_t prev = flux_read_wait(f) >> 1;
  bool ix_prev = false;

  floppy_status_t ret = FLOPPY_ERR_TIMEOUT;
  // Try to read 4-5 revolutions (count each edge, so FLOPPY_READ_TRACK_ATTEMPTS *2)
  for (int ix_edges = 0; ix_edges < FLOPPY_READ_TRACK_ATTEMPTS * 2;) {
    uint16_t value = flux_read_wait(f);
    uint8_t ix = value & 1;
    uint16_t cnt = value >> 1;
    int delta = prev - cnt;
    if (delta < 0) delta += 0x8000;

    if (ix != ix_prev) ix_edges++;  // Index pin status changed
    ix_prev = ix;

    if (mfm_feed(&mfm, delta, &sector)) {
      if (sector.valid && sector.sector_n >= 1 && sector.sector_n <= SECTORS_PER_TRACK) {
        if (sector.track != track) {
          FLOPPY_ERR("[floppy] read sector: wrong track, expected %d got %d\n", track, sector.track);
          ret = FLOPPY_ERR_WRONG_TRACK;
          break;
        }
        if (sector.side != side) {
          FLOPPY_ERR("[floppy] read sector: wrong side, expected %d got %d\n", side, sector.side);
          ret = FLOPPY_ERR_WRONG_SIDE;
          break;
        }
        if (sector.sector_n == sector_n) {
          *out = sector;
          ret = FLOPPY_OK;
          break;
        }
      }
    }
    prev = cnt;
  }

  floppy_flux_read_stop(f);

  if (ret == FLOPPY_ERR_TIMEOUT) {
    FLOPPY_ERR("[floppy] timeout reading track %d side %d sector %d\n", track, side, sector_n);
  }

  return ret;
}

// ============== Public API ==============

void floppy_init(floppy_t *f) {
  // Initialize input pins
  uint inputs[] = {f->pins.index, f->pins.track0, f->pins.write_protect,
                   f->pins.read_data, f->pins.disk_change};
  for (int i = 0; i < 5; i++) {
    gpio_init(inputs[i]);
    gpio_set_dir(inputs[i], GPIO_IN);
    gpio_pull_up(inputs[i]);
  }

  // Initialize output pins (active low, start as high-Z)
  uint outputs[] = {f->pins.drive_select, f->pins.motor_enable, f->pins.direction,
                    f->pins.step, f->pins.write_data, f->pins.write_gate,
                    f->pins.side_select, f->pins.density};
  for (int i = 0; i < 8; i++) {
    gpio_init(outputs[i]);
    gpio_put(outputs[i], 0);
    gpio_set_dir(outputs[i], GPIO_IN);  // Start as high-Z
  }

  // Initialize read PIO
  f->read.pio = pio0;
  f->read.offset = pio_add_program(f->read.pio, &flux_read_program);
  f->read.sm = pio_claim_unused_sm(f->read.pio, true);
  f->read.half = 0;
  flux_read_program_init(f->read.pio, f->read.sm, f->read.offset,
                         f->pins.read_data, f->pins.index);

  pio_sm_clear_fifos(f->read.pio, f->read.sm);
  pio_sm_restart(f->read.pio, f->read.sm);
  pio_sm_set_enabled(f->read.pio, f->read.sm, false);

  // Initialize write PIO
  f->write.pio = pio1;
  f->write.offset = pio_add_program(f->write.pio, &flux_write_program);
  f->write.sm = pio_claim_unused_sm(f->write.pio, true);
  f->write.half = 0;
  flux_write_program_init(f->write.pio, f->write.sm, f->write.offset, f->pins.write_data);

  // Initialize state
  f->track = 0;
  f->track0_confirmed = false;
  f->disk_change_flag = false;
  f->motor_on = false;
  f->selected = false;
  f->auto_motor = true;  // Enable auto motor management by default
  f->last_io_time_ms = 0;

  // Start idle timer for auto motor management
  add_repeating_timer_ms(IDLE_CHECK_INTERVAL_MS, floppy_idle_timer_callback, f, &f->idle_timer);
}

void floppy_select(floppy_t *f, bool on) {
  if (f->selected == on) return;  // No change needed
  gpio_put_oc(f->pins.drive_select, !on);
  f->selected = on;
  sleep_ms(10);
}

void floppy_motor_on(floppy_t *f) {
  if (f->motor_on) return;  // Already on
  gpio_put_oc(f->pins.motor_enable, 0);  // Active low
  f->motor_on = true;
  sleep_ms(750);  // Wait for spinup
}

void floppy_motor_off(floppy_t *f) {
  if (!f->motor_on) return;  // Already off
  gpio_put_oc(f->pins.motor_enable, 1);  // Inactive
  f->motor_on = false;
}

void floppy_set_density(floppy_t *f, bool hd) {
  gpio_put(f->pins.density, hd ? 1 : 0);
  sleep_ms(15);
}

floppy_status_t floppy_seek(floppy_t *f, uint8_t target) {
  if (target == 0) {
    return floppy_seek_track0(f);
  }

  if (target >= FLOPPY_TRACKS) {
    target = FLOPPY_TRACKS - 1;
  }

  while (f->track < target) {
    floppy_step(f, DIR_INWARD);
  }
  while (f->track > target) {
    floppy_step(f, DIR_OUTWARD);
  }

  return FLOPPY_OK;
}

uint8_t floppy_current_track(floppy_t *f) {
  return f->track;
}

bool floppy_at_track0(floppy_t *f) {
  return !gpio_get(f->pins.track0);  // Active low
}

bool floppy_disk_changed(floppy_t *f) {
  // Disk change signal is active low and latched
  // It goes low when a disk is removed/inserted
  // Stepping the head clears the latch
  bool changed = !gpio_get(f->pins.disk_change);

  if (changed) {
    // Clear the latch by stepping the head
    // Step inward then back if not at track 0, or outward then back if at track 0
    if (f->track > 0) {
      floppy_step(f, DIR_OUTWARD);
      floppy_step(f, DIR_INWARD);
    } else {
      floppy_step(f, DIR_INWARD);
      floppy_step(f, DIR_OUTWARD);
    }
  }

  return changed;
}

bool floppy_write_protected(floppy_t *f) {
  return !gpio_get(f->pins.write_protect);  // Active low
}

floppy_status_t floppy_read_sector(floppy_t *f, sector_t *sector) {
  sector->valid = false;
  floppy_prepare(f);
  return floppy_read_internal(f, sector->track, sector->side, sector->sector_n, sector);
}

floppy_status_t floppy_write_track(floppy_t *f, track_t *t) {
  // Check write protection
  if (floppy_write_protected(f)) {
    FLOPPY_ERR("[floppy] write track %d side %d: disk is write protected\n", t->track, t->side);
    return FLOPPY_ERR_WRITE_PROTECTED;
  }

  floppy_prepare(f);

  // Complete any missing sectors by reading from disk
  floppy_status_t status = floppy_complete_track(f, t);
  if (status != FLOPPY_OK) {
    return status;
  }

  // Encode track to flux
  uint8_t flux_buf[200000];
  mfm_encode_t enc;
  mfm_encode_init(&enc, flux_buf, sizeof(flux_buf));
  mfm_encode_track(&enc, t);

  // Seek and select side
  floppy_seek(f, t->track);
  floppy_side_select(f, t->side);

  // Wait for index and write
  floppy_wait_for_index(f);
  floppy_flux_write_start(f);

  for (size_t i = 0; i < enc.pos; i++) {
    pio_sm_put_blocking(f->write.pio, f->write.sm, flux_buf[i]);
  }

  floppy_flux_write_stop(f);

  return FLOPPY_OK;
}

// ============== f12 I/O Callbacks ==============

bool floppy_io_read(void *ctx, sector_t *sector) {
  floppy_t *f = (floppy_t *)ctx;
  return floppy_read_sector(f, sector) == FLOPPY_OK;
}

bool floppy_io_write(void *ctx, track_t *track) {
  floppy_t *f = (floppy_t *)ctx;
  return floppy_write_track(f, track) == FLOPPY_OK;
}

bool floppy_io_disk_changed(void *ctx) {
  floppy_t *f = (floppy_t *)ctx;
  return floppy_disk_changed(f);
}

bool floppy_io_write_protected(void *ctx) {
  floppy_t *f = (floppy_t *)ctx;
  return floppy_write_protected(f);
}
