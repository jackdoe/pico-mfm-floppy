#include "mfm_encode.h"
#include "crc.h"
#include <string.h>

static void mfm_encode_out(mfm_encode_t *e, uint8_t timing) {
  if (e->stopped) return;
  if (e->emit) {
    if (e->pos == SIZE_MAX) {
      e->overflow = true;
      e->stopped = true;
      return;
    }
    if (!e->emit(e->emit_ctx, timing)) {
      e->stopped = true;
      return;
    }
    e->pos++;
    return;
  }
  if (e->pos < e->size) {
    e->buf[e->pos++] = timing;
    return;
  }
  e->overflow = true;
  e->stopped = true;
}

static void mfm_encode_pulse(mfm_encode_t *e, uint8_t timing) {
  if (e->stopped) return;
  if (!e->precomp_shift) {
    mfm_encode_out(e, timing);
    return;
  }
  if (!e->held_valid) {
    e->held = timing;
    e->held_valid = true;
    return;
  }
  uint8_t out = e->held;
  if (!e->held_first && out == MFM_PULSE_SHORT) {
    bool prev_long = e->last_out == MFM_PULSE_LONG;
    bool next_long = timing == MFM_PULSE_LONG;
    if (prev_long && !next_long) {
      out = (uint8_t)((int)out - e->precomp_shift);
    } else if (next_long && !prev_long) {
      out = (uint8_t)((int)out + e->precomp_shift);
    }
  }
  mfm_encode_out(e, out);
  e->last_out = out;
  e->held = timing;
  e->held_first = false;
}

static void mfm_encode_emit(mfm_encode_t *e) {
  if (e->pending_cells <= 1) {
    mfm_encode_pulse(e, MFM_PULSE_SHORT);
  } else if (e->pending_cells == 2) {
    mfm_encode_pulse(e, MFM_PULSE_MEDIUM);
  } else {
    mfm_encode_pulse(e, MFM_PULSE_LONG);
  }
  e->pending_cells = 0;
}

void mfm_encode_init(mfm_encode_t *e, uint8_t *buf, size_t size) {
  if (!e) return;
  memset(e, 0, sizeof(*e));
  e->buf = buf;
  e->size = size;
  if (!buf && size != 0) {
    e->overflow = true;
    e->stopped = true;
  }
}

void mfm_encode_init_emit(mfm_encode_t *e, mfm_emit_fn emit, void *ctx) {
  if (!e) return;
  memset(e, 0, sizeof(*e));
  e->emit = emit;
  e->emit_ctx = ctx;
  if (!emit) e->stopped = true;
}

void mfm_encode_bytes(mfm_encode_t *e, const uint8_t *data, size_t len) {
  if (!e || (!data && len != 0)) {
    if (e) e->stopped = true;
    return;
  }
  for (size_t i = 0; i < len && !e->stopped; i++) {
    uint8_t byte = data[i];
    for (int bit = 7; bit >= 0 && !e->stopped; bit--) {
      int data_bit = (byte >> bit) & 1;
      int clock_bit = e->prev_bit == 0 && data_bit == 0;
      if (clock_bit) mfm_encode_emit(e);
      else e->pending_cells++;
      if (data_bit) mfm_encode_emit(e);
      else e->pending_cells++;
      e->prev_bit = data_bit;
    }
  }
}

void mfm_encode_sync(mfm_encode_t *e) {
  static const uint8_t preamble[12] = {0};
  static const uint8_t sync[] = {
      MFM_PULSE_MEDIUM, MFM_PULSE_LONG, MFM_PULSE_MEDIUM,
      MFM_PULSE_LONG, MFM_PULSE_MEDIUM, MFM_PULSE_SHORT,
      MFM_PULSE_LONG, MFM_PULSE_MEDIUM, MFM_PULSE_LONG,
      MFM_PULSE_MEDIUM, MFM_PULSE_SHORT, MFM_PULSE_LONG,
      MFM_PULSE_MEDIUM, MFM_PULSE_LONG, MFM_PULSE_MEDIUM,
  };
  if (!e || e->stopped) return;
  mfm_encode_bytes(e, preamble, sizeof(preamble));
  for (size_t i = 0; i < sizeof(sync) && !e->stopped; i++) {
    mfm_encode_pulse(e, sync[i]);
  }
  e->prev_bit = 1;
  e->pending_cells = 0;
}

void mfm_encode_gap(mfm_encode_t *e, size_t count) {
  if (!e || e->stopped) return;
  for (size_t i = 0; i < count && !e->stopped; i++) {
    uint8_t gap = MFM_GAP_BYTE;
    mfm_encode_bytes(e, &gap, 1);
  }
}

void mfm_encode_sector(mfm_encode_t *e, uint8_t cylinder, uint8_t head,
                       uint8_t sector, const uint8_t data[DISK_SECTOR_SIZE]) {
  if (!e || e->stopped) return;
  if (!disk_ch_valid(cylinder, head) || !disk_sector_valid(sector) || !data) {
    e->stopped = true;
    return;
  }
  uint8_t address[] = {MFM_ADDR_MARK, cylinder, head, (uint8_t)(sector + 1u), MFM_SIZE_CODE};
  uint16_t address_crc = crc16_mfm(address, sizeof(address));
  uint8_t address_crc_bytes[] = {
      (uint8_t)(address_crc >> 8u), (uint8_t)(address_crc & 0xFFu)};
  uint8_t data_mark = MFM_DATA_MARK;
  uint16_t data_crc = crc16(data, DISK_SECTOR_SIZE, crc16_mfm(&data_mark, 1));
  uint8_t data_crc_bytes[] = {
      (uint8_t)(data_crc >> 8u), (uint8_t)(data_crc & 0xFFu)};

  mfm_encode_sync(e);
  mfm_encode_bytes(e, address, sizeof(address));
  mfm_encode_bytes(e, address_crc_bytes, sizeof(address_crc_bytes));
  mfm_encode_gap(e, 22);
  mfm_encode_sync(e);
  mfm_encode_bytes(e, &data_mark, 1);
  mfm_encode_bytes(e, data, DISK_SECTOR_SIZE);
  mfm_encode_bytes(e, data_crc_bytes, sizeof(data_crc_bytes));
}

size_t mfm_encode_track(mfm_encode_t *e, const track_t *track) {
  if (!e) return 0;
  if (!track || !disk_ch_valid(track->cylinder, track->head) ||
      track->valid != DISK_TRACK_VALID) {
    e->stopped = true;
    return e->pos;
  }
  if (track->cylinder >= MFM_PRECOMP_START_TRACK) {
    e->precomp_shift = MFM_PRECOMP_SHIFT +
                       ((int)track->cylinder - (int)MFM_PRECOMP_START_TRACK) / 13;
    e->held_valid = false;
    e->held_first = true;
  }
  mfm_encode_gap(e, 80);
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK && !e->stopped; sector++) {
    mfm_encode_sector(e, track->cylinder, track->head, sector, track->data[sector]);
    mfm_encode_gap(e, 54);
  }
  if (e->precomp_shift) {
    e->precomp_shift = 0;
    if (e->held_valid && !e->stopped) mfm_encode_out(e, e->held);
    e->held_valid = false;
  }
  return e->pos;
}
