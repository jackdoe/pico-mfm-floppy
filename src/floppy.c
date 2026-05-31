#include "floppy.h"
#include "mfm_decode.h"
#include "mfm_encode.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "flux_read.pio.h"
#include "flux_write.pio.h"

#ifndef FLOPPY_DEBUG
#define FLOPPY_DEBUG 1
#endif

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

#define DIR_INWARD 1
#define DIR_OUTWARD 2
#define IDLE_CHECK_INTERVAL_MS 1000

#ifndef FLOPPY_FLUX_WAIT_TIMEOUT_MS
#define FLOPPY_FLUX_WAIT_TIMEOUT_MS 250
#endif

#ifndef FLOPPY_INDEX_TIMEOUT_MS
#define FLOPPY_INDEX_TIMEOUT_MS 500
#endif

#ifndef FLOPPY_TX_DRAIN_TIMEOUT_MS
#define FLOPPY_TX_DRAIN_TIMEOUT_MS 100
#endif

static void gpio_put_oc(uint pin, bool value);

static bool floppy_idle_timer_callback(struct repeating_timer *t) {
  floppy_t *f = (floppy_t *)t->user_data;

  if (!f->motor_on) {
    return true;
  }

  uint32_t now = to_ms_since_boot(get_absolute_time());
  if (now - f->last_io_time_ms >= FLOPPY_IDLE_TIMEOUT_MS) {
    uint32_t saved = save_and_disable_interrupts();
    gpio_put_oc(f->pins.motor_enable, 1);
    f->motor_on = false;
    gpio_put_oc(f->pins.drive_select, 1);
    f->selected = false;
    restore_interrupts(saved);
  }

  return true;
}

static void floppy_touch(floppy_t *f) {
  if (!f->auto_motor) return;
  uint32_t saved = save_and_disable_interrupts();
  f->last_io_time_ms = to_ms_since_boot(get_absolute_time());
  restore_interrupts(saved);
}

static void floppy_prepare(floppy_t *f) {
  if (!f->auto_motor) return;

  floppy_touch(f);
  floppy_select(f, true);
  floppy_motor_on(f);
}

static void gpio_put_oc(uint pin, bool value) {
  if (value == 0) {
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_OUT);
  } else {
    gpio_set_dir(pin, GPIO_IN);
  }
}

static inline bool flux_data_available(floppy_t *f) {
  return f->read.half_valid || !pio_sm_is_rx_fifo_empty(f->read.pio, f->read.sm);
}

static inline uint16_t flux_read(floppy_t *f) {
  if (f->read.half_valid) {
    f->read.half_valid = false;
    return f->read.half;
  }
  uint32_t pv = pio_sm_get_blocking(f->read.pio, f->read.sm);
  f->read.half = pv >> 16;
  f->read.half_valid = true;
  return pv & 0xffff;
}

static inline bool flux_read_wait(floppy_t *f, uint16_t *out) {
  uint32_t start_ms = to_ms_since_boot(get_absolute_time());
  while (!flux_data_available(f)) {
    if ((uint32_t)(to_ms_since_boot(get_absolute_time()) - start_ms) >= FLOPPY_FLUX_WAIT_TIMEOUT_MS) {
      return false;
    }
    tight_loop_contents();
  }
  *out = flux_read(f);
  return true;
}

static void floppy_flux_read_start(floppy_t *f) {
  pio_sm_set_enabled(f->read.pio, f->read.sm, false);
  pio_sm_clear_fifos(f->read.pio, f->read.sm);
  pio_sm_restart(f->read.pio, f->read.sm);
  f->read.half_valid = false;
  pio_sm_exec(f->read.pio, f->read.sm, pio_encode_set(pio_x, 0));
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

static bool floppy_flux_write_stop(floppy_t *f) {
  uint32_t start_ms = to_ms_since_boot(get_absolute_time());
  while (!pio_sm_is_tx_fifo_empty(f->write.pio, f->write.sm)) {
    if ((uint32_t)(to_ms_since_boot(get_absolute_time()) - start_ms) >= FLOPPY_TX_DRAIN_TIMEOUT_MS) {
      gpio_put_oc(f->pins.write_gate, 1);
      pio_sm_set_enabled(f->write.pio, f->write.sm, false);
      return false;
    }
    tight_loop_contents();
  }
  sleep_us(5);
  gpio_put_oc(f->pins.write_gate, 1);
  pio_sm_set_enabled(f->write.pio, f->write.sm, false);
  return true;
}

static bool floppy_wait_for_index(floppy_t *f) {
  uint32_t start_ms = to_ms_since_boot(get_absolute_time());
  while (!gpio_get(f->pins.index)) {
    if ((uint32_t)(to_ms_since_boot(get_absolute_time()) - start_ms) >= FLOPPY_INDEX_TIMEOUT_MS) {
      return false;
    }
    tight_loop_contents();
  }

  start_ms = to_ms_since_boot(get_absolute_time());
  while (gpio_get(f->pins.index)) {
    if ((uint32_t)(to_ms_since_boot(get_absolute_time()) - start_ms) >= FLOPPY_INDEX_TIMEOUT_MS) {
      return false;
    }
    tight_loop_contents();
  }

  return true;
}

static void floppy_side_select(floppy_t *f, uint8_t side) {
  gpio_put_oc(f->pins.side_select, side == 0 ? 1 : 0);
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
#define FLOPPY_READ_TRACK_ATTEMPTS 15
#define FLOPPY_WRITE_ATTEMPTS 3
#define FLOPPY_HEAD_SETTLE_MS 20

static bool floppy_status_retryable(floppy_status_t status) {
  return status == FLOPPY_ERR_TIMEOUT ||
         status == FLOPPY_ERR_WRONG_TRACK ||
         status == FLOPPY_ERR_WRONG_SIDE;
}

static void floppy_stats_record_failure(floppy_t *f, floppy_status_t status) {
  if (status == FLOPPY_OK) return;
  f->stats.failed++;
  if (status == FLOPPY_ERR_TIMEOUT) f->stats.timeout++;
  else if (status == FLOPPY_ERR_WRONG_TRACK) f->stats.wrong_track++;
  else if (status == FLOPPY_ERR_WRONG_SIDE) f->stats.wrong_side++;
}

static floppy_status_t floppy_jog(floppy_t *f, uint8_t track, uint8_t distance) {
  floppy_touch(f);
  uint8_t away = (track <= distance) ? track + distance : track - distance;
  floppy_status_t status = floppy_seek(f, away);
  if (status != FLOPPY_OK) return status;

  status = floppy_seek(f, track);
  if (status != FLOPPY_OK) return status;

  sleep_ms(FLOPPY_HEAD_SETTLE_MS);
  return FLOPPY_OK;
}

typedef bool (*sector_callback_t)(sector_t *sector, void *ctx);

static floppy_status_t floppy_read_flux(floppy_t *f, int track, int side,
                                        sector_callback_t cb, void *ctx) {
  floppy_touch(f);
  floppy_status_t status = floppy_seek(f, track);
  if (status != FLOPPY_OK) {
    return status;
  }
  floppy_side_select(f, side);
  floppy_flux_read_start(f);

  mfm_t mfm;
  sector_t sector = {0};
  mfm_init(&mfm);
  mfm_reset(&mfm);

  uint16_t initial;
  if (!flux_read_wait(f, &initial)) {
    floppy_flux_read_stop(f);
    return FLOPPY_ERR_TIMEOUT;
  }
  uint16_t prev = initial >> 1;
  bool ix_prev = initial & 1;
  floppy_status_t res = FLOPPY_ERR_TIMEOUT;

  for (int ix_edges = 0; ix_edges < FLOPPY_READ_TRACK_ATTEMPTS * 2;) {
    uint16_t value;
    if (!flux_read_wait(f, &value)) {
      res = FLOPPY_ERR_TIMEOUT;
      break;
    }
    uint8_t ix = value & 1;
    uint16_t cnt = value >> 1;
    int delta = prev - cnt;
    if (delta < 0) delta += 0x8000;

    if (ix != ix_prev) ix_edges++;
    ix_prev = ix;

    if (mfm_feed(&mfm, delta, &sector)) {
      if (sector.valid && sector.sector_n >= 1 && sector.sector_n <= SECTORS_PER_TRACK) {
        if (sector.track != track) {
          FLOPPY_ERR("[floppy] wrong track: expected %d, got %d\n", track, sector.track);
          floppy_flux_read_stop(f);
          return FLOPPY_ERR_WRONG_TRACK;
        }
        if (sector.side != side) {
          FLOPPY_ERR("[floppy] wrong side: expected %d, got %d\n", side, sector.side);
          floppy_flux_read_stop(f);
          return FLOPPY_ERR_WRONG_SIDE;
        }
        if (cb(&sector, ctx)) {
          res = FLOPPY_OK;
          break;
        }
      }
    }
    prev = cnt;
  }

  floppy_flux_read_stop(f);
  return res;
}

static floppy_status_t floppy_read_recover(floppy_t *f, int track, int side,
                                           sector_callback_t cb, void *ctx) {
  floppy_status_t res = floppy_read_flux(f, track, side, cb, ctx);
  if (res == FLOPPY_OK || !floppy_status_retryable(res)) return res;

  static const uint8_t jog_distance[] = {10, 20};
  for (unsigned i = 0; i < sizeof(jog_distance); i++) {
    f->stats.retries++;
    res = floppy_jog(f, track, jog_distance[i]);
    if (res != FLOPPY_OK) return res;
    res = floppy_read_flux(f, track, side, cb, ctx);
    if (res == FLOPPY_OK) {
      f->stats.recovered++;
      return res;
    }
    if (!floppy_status_retryable(res)) return res;
  }
  return res;
}

struct complete_track_ctx {
  track_t *t;
};

static bool complete_track_cb(sector_t *sector, void *ctx) {
  struct complete_track_ctx *c = (struct complete_track_ctx *)ctx;
  track_t *t = c->t;

  if (!t->sectors[sector->sector_n - 1].valid) {
    t->sectors[sector->sector_n - 1] = *sector;
  }

  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    if (!t->sectors[i].valid) return false;
  }
  return true;
}

static floppy_status_t floppy_complete_track(floppy_t *f, track_t *t) {
  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    if (!t->sectors[i].valid) goto need_read;
  }
  return FLOPPY_OK;

need_read:;
  struct complete_track_ctx ctx = { .t = t };
  floppy_status_t res = floppy_read_recover(f, t->track, t->side, complete_track_cb, &ctx);

  if (res == FLOPPY_ERR_TIMEOUT) {
    FLOPPY_ERR("[floppy] timeout reading track %d side %d, missing sectors:", t->track, t->side);
    for (int i = 0; i < SECTORS_PER_TRACK; i++) {
      if (!t->sectors[i].valid) {
        FLOPPY_ERR(" %d", i + 1);
      }
    }
    FLOPPY_ERR("\n");
  }
  return res;
}

struct read_sector_ctx {
  int sector_n;
  sector_t *out;
};

static bool read_sector_cb(sector_t *sector, void *ctx) {
  struct read_sector_ctx *c = (struct read_sector_ctx *)ctx;
  if (sector->sector_n == c->sector_n) {
    *c->out = *sector;
    return true;
  }
  return false;
}

void floppy_init(floppy_t *f) {
  uint inputs[] = {f->pins.index, f->pins.track0, f->pins.write_protect,
                   f->pins.read_data, f->pins.disk_change};
  for (int i = 0; i < 5; i++) {
    gpio_init(inputs[i]);
    gpio_set_dir(inputs[i], GPIO_IN);
    gpio_pull_up(inputs[i]);
  }

  uint outputs[] = {f->pins.drive_select, f->pins.motor_enable, f->pins.direction,
                    f->pins.step, f->pins.write_data, f->pins.write_gate,
                    f->pins.side_select, f->pins.density};
  for (int i = 0; i < 8; i++) {
    gpio_init(outputs[i]);
    gpio_put(outputs[i], 0);
    gpio_set_dir(outputs[i], GPIO_IN);
  }

  f->read.pio = pio0;
  f->read.offset = pio_add_program(f->read.pio, &flux_read_program);
  f->read.sm = pio_claim_unused_sm(f->read.pio, true);
  f->read.half_valid = false;
  flux_read_program_init(f->read.pio, f->read.sm, f->read.offset,
                         f->pins.read_data, f->pins.index);

  pio_sm_clear_fifos(f->read.pio, f->read.sm);
  pio_sm_restart(f->read.pio, f->read.sm);
  pio_sm_set_enabled(f->read.pio, f->read.sm, false);

  f->write.pio = pio1;
  f->write.offset = pio_add_program(f->write.pio, &flux_write_program);
  f->write.sm = pio_claim_unused_sm(f->write.pio, true);
  f->write.half_valid = false;
  flux_write_program_init(f->write.pio, f->write.sm, f->write.offset, f->pins.write_data);

  f->track = 0;
  f->track0_confirmed = false;
  f->disk_change_flag = false;
  f->motor_on = false;
  f->selected = false;
  f->auto_motor = true;
  f->last_io_time_ms = 0;

  add_repeating_timer_ms(IDLE_CHECK_INTERVAL_MS, floppy_idle_timer_callback, f, &f->idle_timer);
}

void floppy_select(floppy_t *f, bool on) {
  if (f->selected == on) return;
  gpio_put_oc(f->pins.drive_select, !on);
  f->selected = on;
  sleep_ms(10);
}

void floppy_motor_on(floppy_t *f) {
  if (f->motor_on) return;
  gpio_put_oc(f->pins.motor_enable, 0);
  f->motor_on = true;
  sleep_ms(750);
}

void floppy_motor_off(floppy_t *f) {
  if (!f->motor_on) return;
  gpio_put_oc(f->pins.motor_enable, 1);
  f->motor_on = false;
}

void floppy_set_density(floppy_t *f, bool hd) {
  gpio_put(f->pins.density, hd ? 1 : 0);
  sleep_ms(15);
}

floppy_status_t floppy_seek(floppy_t *f, uint8_t target) {
  if (target >= FLOPPY_TRACKS) {
    return FLOPPY_ERR_WRONG_TRACK;
  }

  if (!f->track0_confirmed) {
    floppy_status_t s = floppy_seek_track0(f);
    if (s != FLOPPY_OK) return s;
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
  return !gpio_get(f->pins.track0);
}

bool floppy_disk_changed(floppy_t *f) {
  return !gpio_get(f->pins.disk_change);
}

bool floppy_write_protected(floppy_t *f) {
  return !gpio_get(f->pins.write_protect);
}

void floppy_stats_reset(floppy_t *f) {
  memset(&f->stats, 0, sizeof(f->stats));
}

floppy_stats_t floppy_stats(floppy_t *f) {
  return f->stats;
}

floppy_status_t floppy_read_sector(floppy_t *f, sector_t *sector) {
  sector->valid = false;
  floppy_prepare(f);
  f->stats.reads++;

  struct read_sector_ctx ctx = { .sector_n = sector->sector_n, .out = sector };
  floppy_status_t st = floppy_read_recover(f, sector->track, sector->side, read_sector_cb, &ctx);

  if (st == FLOPPY_ERR_TIMEOUT) {
    FLOPPY_ERR("[floppy] timeout reading track %d side %d sector %d\n",
               sector->track, sector->side, sector->sector_n);
  }
  if (st != FLOPPY_OK) floppy_stats_record_failure(f, st);
  return st;
}

struct verify_ctx {
  const track_t *expected;
  bool verified[SECTORS_PER_TRACK];
};

static bool verify_track_cb(sector_t *sector, void *ctx) {
  struct verify_ctx *v = (struct verify_ctx *)ctx;
  int idx = sector->sector_n - 1;

  if (!v->verified[idx] &&
      sector->track == v->expected->sectors[idx].track &&
      sector->side == v->expected->sectors[idx].side &&
      memcmp(sector->data, v->expected->sectors[idx].data, SECTOR_SIZE) == 0) {
    v->verified[idx] = true;
  }

  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    if (!v->verified[i]) return false;
  }
  return true;
}

floppy_status_t floppy_write_track(floppy_t *f, track_t *t) {
  if (floppy_write_protected(f)) {
    FLOPPY_ERR("[floppy] write track %d side %d: disk is write protected\n", t->track, t->side);
    return FLOPPY_ERR_WRITE_PROTECTED;
  }

  floppy_prepare(f);

  floppy_status_t status = floppy_complete_track(f, t);
  if (status != FLOPPY_OK) {
    return status;
  }

  mfm_encode_t enc;
  mfm_encode_init(&enc, f->flux_buf, sizeof(f->flux_buf));
  mfm_encode_track(&enc, t);
  if (enc.overflow) {
    FLOPPY_ERR("[floppy] encode overflow track %d side %d\n", t->track, t->side);
    return FLOPPY_ERR_ENCODE_OVERFLOW;
  }

  for (int attempt = 0; attempt < FLOPPY_WRITE_ATTEMPTS; attempt++) {
    if (attempt == 2) {
      status = floppy_seek_track0(f);
      if (status != FLOPPY_OK) {
        return status;
      }
    }

    status = floppy_seek(f, t->track);
    if (status != FLOPPY_OK) {
      return status;
    }
    floppy_side_select(f, t->side);
    if (!floppy_wait_for_index(f)) {
      return FLOPPY_ERR_TIMEOUT;
    }
    floppy_flux_write_start(f);
    for (size_t i = 0; i < enc.pos; i++) {
      pio_sm_put_blocking(f->write.pio, f->write.sm, f->flux_buf[i]);
    }
    if (!floppy_flux_write_stop(f)) {
      return FLOPPY_ERR_TIMEOUT;
    }

    struct verify_ctx vctx = { .expected = t };
    for (int verify = 0; verify < 3; verify++) {
      if (verify > 0) {
        status = floppy_jog(f, t->track, 10);
        if (status != FLOPPY_OK) {
          return status;
        }
      }

      floppy_status_t verify_status =
          floppy_read_flux(f, t->track, t->side, verify_track_cb, &vctx);
      if (verify_status == FLOPPY_OK) {
        return FLOPPY_OK;
      }
      if (!floppy_status_retryable(verify_status)) {
        return verify_status;
      }
    }

    FLOPPY_ERR("[floppy] verify failed track %d side %d attempt %d, bad sectors:",
               t->track, t->side, attempt + 1);
    for (int i = 0; i < SECTORS_PER_TRACK; i++) {
      if (!vctx.verified[i]) {
        FLOPPY_ERR(" %d", i + 1);
      }
    }
    FLOPPY_ERR("\n");
  }

  return FLOPPY_ERR_VERIFY;
}

floppy_status_t floppy_read_track(floppy_t *f, track_t *t) {
  floppy_prepare(f);
  for (int i = 0; i < SECTORS_PER_TRACK; i++)
    t->sectors[i].valid = false;
  return floppy_complete_track(f, t);
}

bool floppy_io_read_track(void *ctx, track_t *track) {
  floppy_t *f = (floppy_t *)ctx;
  return floppy_read_track(f, track) == FLOPPY_OK;
}

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
