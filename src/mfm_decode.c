#include "mfm_decode.h"
#include "crc.h"
#include "mfm_encode.h"
#include <string.h>

static const int8_t sync_pattern[] = {1, 2, 1, 2, 1, 0, 2, 1, 2, 1, 0, 2, 1, 2, 1};

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
  if (delta < MFM_PULSE_FLOOR) return -1;
  if (delta <= m->T2_max) {
    if ((m->state == MFM_DATA || m->state == MFM_CLOCK) && m->t_cell > 0 &&
        delta <= m->t_cell + (m->t_cell >> 3)) {
      int difference = (int)delta - (int)m->t_cell;
      int adjustment = difference >= 0 ? (difference + 8) / 16
                                       : -((-difference + 7) / 16);
      m->t_cell = (uint16_t)((int)m->t_cell + adjustment);
      m->T2_max = (uint16_t)((uint32_t)m->t_cell * 5u / 4u);
      m->T3_max = (uint16_t)((uint32_t)m->t_cell * 7u / 4u);
    }
    return MFM_SHORT;
  }
  if (delta <= m->T3_max) return MFM_MEDIUM;
  if (delta < MFM_PULSE_CEILING) return MFM_LONG;
  return -1;
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
  m->T2_max = 57;
  m->T3_max = 82;
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
      mfm_abort(m);
    }
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
        m->t_cell = (uint16_t)(m->preamble_sum / m->short_count);
        m->T2_max = (uint16_t)((uint32_t)m->t_cell * 5u / 4u);
        m->T3_max = (uint16_t)((uint32_t)m->t_cell * 7u / 4u);
        if (pulse == sync_pattern[0]) {
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
      if (pulse != sync_pattern[m->sync_stage]) {
        if (m->record_state == MFM_EXPECT_DATA) m->format_errors++;
        mfm_abort(m);
        if (pulse == MFM_SHORT) {
          m->short_count = 1;
          m->preamble_sum = delta;
        }
        return false;
      }
      m->sync_stage++;
      if (m->sync_stage < sizeof(sync_pattern)) return false;
      m->syncs_found++;
      m->state = MFM_DATA;
      m->byte_acc = 0;
      m->bit_count = 0;
      m->buf_pos = 0;
      m->bytes_expected = 0;
      m->crc = 0xFFFF;
      m->crc = crc16_update(m->crc, 0xA1);
      m->crc = crc16_update(m->crc, 0xA1);
      m->crc = crc16_update(m->crc, 0xA1);
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
