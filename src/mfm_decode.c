#include "mfm_decode.h"
#include "crc.h"
#include <string.h>
#include <stdio.h>

static int mfm_classify(mfm_t *m, uint16_t delta) {
  if (delta < MFM_PULSE_FLOOR) return -1;
  if (delta <= m->T2_max) {
    if (m->state >= MFM_DATA && m->t_cell > 0
        && delta <= m->t_cell + (m->t_cell >> 3)) {
      m->t_cell += ((int)delta - (int)m->t_cell + 8) >> 4;
      m->T2_max = m->t_cell * 5 / 4;
      m->T3_max = m->t_cell * 7 / 4;
    }
    return MFM_SHORT;
  }
  if (delta <= m->T3_max) return MFM_MEDIUM;
  if (delta < MFM_PULSE_CEILING) return MFM_LONG;
  return -1;
}

static void mfm_push_bit(mfm_t *m, int bit) {
  m->byte_acc = (m->byte_acc << 1) | (bit ? 1 : 0);
  m->bit_count++;

  if (m->bit_count >= 8) {
    if (m->buf_pos < sizeof(m->buf)) {
      m->buf[m->buf_pos++] = m->byte_acc;
    } else {
      m->overflow = true;
    }
    m->crc = crc16_update(m->crc, m->byte_acc);
    m->bit_count = 0;
    m->byte_acc = 0;
  }
}

static const int8_t sync_pattern[15] = {1,2,1,2,1,0,2,1,2,1,0,2,1,2,1};

void mfm_init(mfm_t *m) {
  memset(m, 0, sizeof(*m));
  m->state = MFM_HUNT;
  m->T2_max = 57;
  m->T3_max = 82;
}

void mfm_reset(mfm_t *m) {
  m->state = MFM_HUNT;
  m->short_count = 0;
  m->preamble_sum = 0;
  m->sync_stage = 0;
}

bool mfm_feed(mfm_t *m, uint16_t delta, sector_t *out) {
  int p = mfm_classify(m, delta);
  if (p < 0) return false;

  switch (m->state) {
    case MFM_HUNT:
      if (p == MFM_SHORT) {
        m->short_count++;
        m->preamble_sum += delta;
      } else {
        if (m->short_count >= MFM_MIN_PREAMBLE) {
          m->t_cell = m->preamble_sum / m->short_count;
          m->T2_max = m->t_cell * 5 / 4;
          m->T3_max = m->t_cell * 7 / 4;
          m->state = MFM_SYNCING;
          m->sync_stage = 0;
          if (p == MFM_MEDIUM) {
            m->sync_stage = 1;
          } else {
            m->state = MFM_HUNT;
          }
        }
        m->short_count = 0;
        m->preamble_sum = 0;
      }
      break;

    case MFM_SYNCING:
      if (p == sync_pattern[m->sync_stage]) {
        m->sync_stage++;
        if (m->sync_stage >= 15) {
          m->syncs_found++;
          m->state = MFM_DATA;
          m->byte_acc = 0;
          m->bit_count = 0;
          m->buf_pos = 0;
          m->bytes_expected = 0;
          m->overflow = false;
          m->crc = 0xFFFF;
          m->crc = crc16_update(m->crc, 0xA1);
          m->crc = crc16_update(m->crc, 0xA1);
          m->crc = crc16_update(m->crc, 0xA1);
        }
      } else {
        if (p == MFM_SHORT) m->short_count = 1;
        m->state = MFM_HUNT;
      }
      break;

    case MFM_DATA:
      switch (p) {
        case MFM_SHORT:
          mfm_push_bit(m, 1);
          break;
        case MFM_MEDIUM:
          mfm_push_bit(m, 0);
          mfm_push_bit(m, 0);
          m->state = MFM_CLOCK;
          break;
        case MFM_LONG:
          mfm_push_bit(m, 0);
          mfm_push_bit(m, 1);
          break;
      }
      goto check_record;

    case MFM_CLOCK:
      switch (p) {
        case MFM_SHORT:
          mfm_push_bit(m, 0);
          break;
        case MFM_MEDIUM:
          mfm_push_bit(m, 1);
          m->state = MFM_DATA;
          break;
        case MFM_LONG:
          mfm_reset(m);
          return false;
      }
      goto check_record;
  }

  return false;

check_record:
  if (m->buf_pos == 1 && m->bytes_expected == 0) {
    uint8_t mark = m->buf[0];
    if (mark == MFM_ADDR_MARK) {
      m->bytes_expected = 7;
    } else if (mark == MFM_DATA_MARK || mark == MFM_DELETED_MARK) {
      if (m->have_pending_addr) {
        m->bytes_expected = 1 + (128 << m->pending_size_code) + 2;
      } else {
        m->bytes_expected = 515;
      }
    } else {
      mfm_reset(m);
      return false;
    }
  }

  if (m->bytes_expected > 0 && m->buf_pos >= m->bytes_expected) {
    uint8_t mark = m->buf[0];
    bool crc_ok = (m->crc == 0);

    if (mark == MFM_ADDR_MARK) {
      if (crc_ok) {
        m->pending_track = m->buf[1];
        m->pending_side = m->buf[2];
        m->pending_sector = m->buf[3];
        uint8_t size_code = m->buf[4] & 0x03;
        m->pending_size_code = (size_code > 2) ? 2 : size_code;
        m->have_pending_addr = true;
      } else {
        m->crc_errors++;
        m->have_pending_addr = false;
      }
      mfm_reset(m);
    } else if ((mark == MFM_DATA_MARK || mark == MFM_DELETED_MARK) && m->have_pending_addr) {
      uint16_t size = 128 << m->pending_size_code;
      out->track = m->pending_track;
      out->side = m->pending_side;
      out->sector_n = m->pending_sector;
      out->size_code = m->pending_size_code;
      out->valid = crc_ok && !m->overflow;

      uint16_t copy_size = size;
      if (copy_size > SECTOR_SIZE) copy_size = SECTOR_SIZE;
      if (copy_size > m->buf_pos - 1) copy_size = m->buf_pos - 1;
      if (copy_size > 0) {
        memcpy(out->data, &m->buf[1], copy_size);
      }

      m->sectors_read++;
      if (!crc_ok) m->crc_errors++;

      m->have_pending_addr = false;
      mfm_reset(m);
      return true;
    } else {
      mfm_reset(m);
    }
  }

  return false;
}

void mfm_print_stats(mfm_t *m) {
  printf("\n=== MFM Stats ===\n");
  printf("Syncs found:   %u\n", m->syncs_found);
  printf("Sectors read:  %u\n", m->sectors_read);
  printf("CRC errors:    %u\n", m->crc_errors);
  printf("=================\n");
}
