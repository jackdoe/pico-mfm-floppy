#include "mfm.h"
#include "crc.h"
#include <string.h>

static const uint8_t mfm_sync_pattern[] = {
    MFM_MEDIUM, MFM_LONG, MFM_MEDIUM, MFM_LONG, MFM_MEDIUM,
    MFM_SHORT, MFM_LONG, MFM_MEDIUM, MFM_LONG, MFM_MEDIUM,
    MFM_SHORT, MFM_LONG, MFM_MEDIUM, MFM_LONG, MFM_MEDIUM,
};

static const uint8_t mfm_pulse[] = {
    MFM_PULSE_SHORT, MFM_PULSE_MEDIUM, MFM_PULSE_LONG,
};

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
  int shift = 0;
  if (e->held == MFM_PULSE_SHORT && timing > MFM_PULSE_SHORT) {
    shift = -e->precomp_shift;
  } else if (e->held > MFM_PULSE_SHORT && timing == MFM_PULSE_SHORT) {
    shift = e->precomp_shift;
  }
  mfm_encode_out(e, (uint8_t)((int)e->held + shift - e->edge_shift));
  e->edge_shift = (int8_t)shift;
  e->held = timing;
}

static void mfm_encode_cells(mfm_encode_t *e) {
  mfm_pulse_t pulse = e->pending_cells <= 1 ? MFM_SHORT
                      : e->pending_cells == 2 ? MFM_MEDIUM
                                              : MFM_LONG;
  mfm_encode_pulse(e, mfm_pulse[pulse]);
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
      if (clock_bit) mfm_encode_cells(e);
      else e->pending_cells++;
      if (data_bit) mfm_encode_cells(e);
      else e->pending_cells++;
      e->prev_bit = data_bit;
    }
  }
}

void mfm_encode_sync(mfm_encode_t *e) {
  static const uint8_t preamble[12] = {0};
  if (!e || e->stopped) return;
  mfm_encode_bytes(e, preamble, sizeof(preamble));
  for (size_t i = 0; i < sizeof(mfm_sync_pattern) && !e->stopped; i++) {
    mfm_encode_pulse(e, mfm_pulse[mfm_sync_pattern[i]]);
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
  uint16_t address_crc = crc16(address, sizeof(address), MFM_CRC_INIT);
  uint8_t address_crc_bytes[] = {
      (uint8_t)(address_crc >> 8u), (uint8_t)(address_crc & 0xFFu)};
  uint8_t data_mark = MFM_DATA_MARK;
  uint16_t data_crc = crc16(data, DISK_SECTOR_SIZE, crc16_update(MFM_CRC_INIT, data_mark));
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
    e->precomp_shift = MFM_PRECOMP_SHIFT;
    e->held_valid = false;
    e->edge_shift = 0;
  }
  mfm_encode_gap(e, 80);
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK && !e->stopped; sector++) {
    mfm_encode_sector(e, track->cylinder, track->head, sector, track->data[sector]);
    mfm_encode_gap(e, 54);
  }
  if (e->precomp_shift) {
    e->precomp_shift = 0;
    if (e->held_valid && !e->stopped) {
      mfm_encode_out(e, (uint8_t)((int)e->held - e->edge_shift));
    }
    e->held_valid = false;
  }
  return e->pos;
}

static void mfm_set_cell(mfm_t *m, uint32_t cell_q8) {
  uint32_t minimum = MFM_PULSE_FLOOR * 256u;
  uint32_t maximum = MFM_CELL_TICKS * 5u / 4u * 256u;
  m->cell_q8 = cell_q8 < minimum ? minimum
                : cell_q8 > maximum ? maximum : cell_q8;
}

static void mfm_hunt(mfm_t *m) {
  m->state = MFM_HUNT;
  m->short_count = 0;
  m->preamble_sum = 0;
  m->sync_stage = 0;
  m->buf_pos = 0;
  m->bytes_expected = 0;
  m->bit_count = 0;
  m->byte_acc = 0;
}

static void mfm_abort(mfm_t *m) {
  mfm_hunt(m);
  m->record_state = MFM_EXPECT_ID;
  m->pending_pulses = 0;
}

static int mfm_classify(mfm_t *m, uint16_t delta) {
  if (delta < MFM_PULSE_FLOOR || delta >= MFM_PULSE_CEILING) return -1;
  int pulse = delta <= mfm_short_limit(m) ? MFM_SHORT
                : delta <= mfm_medium_limit(m) ? MFM_MEDIUM : MFM_LONG;
  if (m->state == MFM_DATA || m->state == MFM_CLOCK) {
    uint32_t observed = (uint32_t)delta * 512u / ((uint32_t)pulse + 2u);
    int32_t difference = (int32_t)observed - (int32_t)m->cell_q8;
    int32_t limit = (int32_t)(m->cell_q8 / 8u);
    if (difference >= -limit && difference <= limit) {
      mfm_set_cell(m, (uint32_t)((int32_t)m->cell_q8 + difference / 16));
    }
  }
  return pulse;
}

static bool mfm_push_bit(mfm_t *m, int bit) {
  m->byte_acc = (uint8_t)(((uint32_t)m->byte_acc << 1u) |
                          (bit != 0 ? 1u : 0u));
  m->bit_count++;
  if (m->bit_count < 8) return true;
  if (m->buf_pos >= sizeof(m->buf)) return false;
  m->buf[m->buf_pos++] = m->byte_acc;
  m->crc = crc16_update(m->crc, m->byte_acc);
  m->bit_count = 0;
  m->byte_acc = 0;
  return true;
}

void mfm_init(mfm_t *m) {
  if (!m) return;
  memset(m, 0, sizeof(*m));
  mfm_set_cell(m, MFM_CELL_TICKS * 256u);
  mfm_abort(m);
}

void mfm_reset(mfm_t *m) {
  if (!m) return;
  mfm_abort(m);
}

bool mfm_feed(mfm_t *m, uint16_t delta, mfm_sector_t *out) {
  if (!m || !out) return false;
  if (m->record_state == MFM_EXPECT_DATA && ++m->pending_pulses > MFM_ID_DATA_MAX_PULSES) {
    m->format_errors++;
    mfm_abort(m);
  }

  int pulse = mfm_classify(m, delta);
  if (pulse < 0) {
    if (m->state != MFM_HUNT || m->record_state != MFM_EXPECT_ID) {
      m->format_errors++;
    }
    mfm_abort(m);
    return false;
  }

  switch (m->state) {
    case MFM_HUNT:
      if (pulse == MFM_SHORT) {
        if (m->short_count == UINT16_MAX ||
            m->preamble_sum > UINT32_MAX - delta) {
          m->short_count = 1;
          m->preamble_sum = delta;
        } else {
          m->short_count++;
          m->preamble_sum += delta;
        }
        return false;
      }
      if (m->short_count >= MFM_MIN_PREAMBLE) {
        mfm_set_cell(m, (uint32_t)((uint64_t)m->preamble_sum * 256u / m->short_count));
        pulse = mfm_classify(m, delta);
        if (pulse == mfm_sync_pattern[0]) {
          m->state = MFM_SYNCING;
          m->sync_stage = 1;
        } else {
          if (m->record_state == MFM_EXPECT_DATA) m->format_errors++;
          mfm_abort(m);
        }
      }
      m->short_count = 0;
      m->preamble_sum = 0;
      return false;

    case MFM_SYNCING:
      if (pulse != mfm_sync_pattern[m->sync_stage]) {
        if (m->record_state == MFM_EXPECT_DATA) m->format_errors++;
        mfm_abort(m);
        if (pulse == MFM_SHORT) {
          m->short_count = 1;
          m->preamble_sum = delta;
        }
        return false;
      }
      m->sync_stage++;
      if (m->sync_stage < sizeof(mfm_sync_pattern)) return false;
      m->syncs_found++;
      m->state = MFM_DATA;
      m->byte_acc = 0;
      m->bit_count = 0;
      m->buf_pos = 0;
      m->bytes_expected = 0;
      m->crc = MFM_CRC_INIT;
      return false;

    case MFM_DATA:
      if (pulse == MFM_SHORT) {
        if (!mfm_push_bit(m, 1)) goto malformed;
      } else if (pulse == MFM_MEDIUM) {
        if (!mfm_push_bit(m, 0) || !mfm_push_bit(m, 0)) goto malformed;
        m->state = MFM_CLOCK;
      } else {
        if (!mfm_push_bit(m, 0) || !mfm_push_bit(m, 1)) goto malformed;
      }
      break;

    case MFM_CLOCK:
      if (pulse == MFM_SHORT) {
        if (!mfm_push_bit(m, 0)) goto malformed;
      } else if (pulse == MFM_MEDIUM) {
        if (!mfm_push_bit(m, 1)) goto malformed;
        m->state = MFM_DATA;
      } else {
        goto malformed;
      }
      break;
  }

  if (m->buf_pos == 1 && m->bytes_expected == 0) {
    uint8_t mark = m->buf[0];
    if (mark == MFM_ADDR_MARK) {
      if (m->record_state == MFM_EXPECT_DATA) {
        m->format_errors++;
        mfm_abort(m);
        return false;
      }
      m->record_state = MFM_EXPECT_ID;
      m->pending_pulses = 0;
      m->bytes_expected = 7;
    } else if ((mark == MFM_DATA_MARK || mark == MFM_DELETED_MARK) &&
               m->record_state == MFM_EXPECT_DATA) {
      m->record_state = MFM_READING_DATA;
      m->bytes_expected = DISK_SECTOR_SIZE + 3u;
    } else {
      goto malformed;
    }
  }

  if (m->bytes_expected == 0 || m->buf_pos < m->bytes_expected) return false;

  if (m->buf[0] == MFM_ADDR_MARK) {
    bool crc_ok = m->crc == 0;
    bool geometry_ok = disk_ch_valid(m->buf[1], m->buf[2]) &&
                       m->buf[3] >= 1 && m->buf[3] <= DISK_SECTORS_PER_TRACK &&
                       m->buf[4] == MFM_SIZE_CODE;
    if (!crc_ok) m->crc_errors++;
    if (!geometry_ok) m->format_errors++;
    if (!crc_ok || !geometry_ok) {
      mfm_abort(m);
      return false;
    }
    m->pending_cylinder = m->buf[1];
    m->pending_head = m->buf[2];
    m->pending_sector = (uint8_t)(m->buf[3] - 1u);
    m->record_state = MFM_EXPECT_DATA;
    m->pending_pulses = 0;
    mfm_hunt(m);
    return false;
  }

  if (m->crc != 0) {
    m->crc_errors++;
    mfm_abort(m);
    return false;
  }
  out->cylinder = m->pending_cylinder;
  out->head = m->pending_head;
  out->sector = m->pending_sector;
  memcpy(out->data, &m->buf[1], DISK_SECTOR_SIZE);
  m->sectors_read++;
  mfm_abort(m);
  return true;

malformed:
  m->format_errors++;
  mfm_abort(m);
  return false;
}
