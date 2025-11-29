#include "mfm_decode.h"
#include "crc.h"
#include <string.h>
#include <stdio.h>

// ============== Internal helpers ==============

// Pulse classification
// Returns: 0=Short(2T), 1=Medium(3T), 2=Long(4T), -1=Invalid
static int mfm_classify(mfm_t *m, uint16_t delta) {
  if (delta < 35) return -1;           // Noise
  if (delta <= m->T2_max) return 0;    // Short (2T)
  if (delta <= m->T3_max) return 1;    // Medium (3T)
  if (delta < 120) return 2;           // Long (4T)
  return -1;                           // Too long
}

// Push a bit into the byte accumulator
static void mfm_push_bit(mfm_t *m, int bit) {
  m->byte_acc = (m->byte_acc << 1) | (bit ? 1 : 0);
  m->bit_count++;

  // When we've accumulated 8 bits, store the byte
  if (m->bit_count >= 8) {
    if (m->buf_pos < sizeof(m->buf)) {
      m->buf[m->buf_pos++] = m->byte_acc;
    }
    m->crc = crc16_update(m->crc, m->byte_acc);
    m->bit_count = 0;
    m->byte_acc = 0;
  }
}

// Expected sync pattern after preamble: M L M L M S L M L M S L M L M
static const int8_t sync_pattern[15] = {1,2,1,2,1,0,2,1,2,1,0,2,1,2,1};

// ============== Public API ==============

void mfm_init(mfm_t *m) {
  memset(m, 0, sizeof(*m));
  m->state = MFM_HUNT;
  // Thresholds based on real hardware histogram:
  //   Short (2T) peaks at ~47, Medium (3T) at ~70, Long (4T) at ~95
  // Set boundaries at midpoints between peaks:
  //   Short/Medium boundary: ~57 (between 50 and 65)
  //   Medium/Long boundary:  ~82 (between 75 and 90)
  m->T2_max = 57;    // <= 57 = Short (2T)
  m->T3_max = 82;    // <= 82 = Medium (3T), > 82 = Long (4T)
}

void mfm_reset(mfm_t *m) {
  m->state = MFM_HUNT;
  m->short_count = 0;
  m->sync_stage = 0;
}

bool mfm_feed(mfm_t *m, uint16_t delta, sector_t *out) {
  int p = mfm_classify(m, delta);
  if (p < 0) return false;  // Invalid pulse, ignore

  switch (m->state) {
    case MFM_HUNT:
      // Look for preamble (consecutive short pulses)
      if (p == 0) {
        m->short_count++;
      } else {
        if (m->short_count >= MFM_MIN_PREAMBLE) {
          // Had enough shorts, check for sync pattern
          m->state = MFM_SYNCING;
          m->sync_stage = 0;
          if (p == 1) {  // First should be M
            m->sync_stage = 1;
          } else {
            m->state = MFM_HUNT;
          }
        }
        m->short_count = 0;
      }
      break;

    case MFM_SYNCING:
      // Verify the sync pattern
      if (p == sync_pattern[m->sync_stage]) {
        m->sync_stage++;
        if (m->sync_stage >= 15) {
          // Full sync pattern matched!
          m->syncs_found++;
          m->state = MFM_DATA;  // After sync, we're at DATA position
          m->byte_acc = 0;
          m->bit_count = 0;
          m->buf_pos = 0;
          m->bytes_expected = 0;
          // CRC includes 3x 0xA1 sync bytes
          m->crc = 0xFFFF;
          m->crc = crc16_update(m->crc, 0xA1);
          m->crc = crc16_update(m->crc, 0xA1);
          m->crc = crc16_update(m->crc, 0xA1);
        }
      } else {
        if (p == 0) m->short_count = 1;
        m->state = MFM_HUNT;
      }
      break;

    case MFM_DATA:
      // At DATA position - transition came from a data bit
      // Reference: mfm.txt assembly l0010
      switch (p) {
        case 0:  // Short: data bit is 1, stay at DATA
          mfm_push_bit(m, 1);
          // state stays MFM_DATA
          break;
        case 1:  // Medium: data bit is 0, next is 0, go to CLOCK
          mfm_push_bit(m, 0);
          mfm_push_bit(m, 0);
          m->state = MFM_CLOCK;
          break;
        case 2:  // Long: data bit is 0, then 1, stay at DATA
          mfm_push_bit(m, 0);
          mfm_push_bit(m, 1);
          // state stays MFM_DATA
          break;
      }
      goto check_record;

    case MFM_CLOCK:
      // At CLOCK position - transition came from a clock bit
      // Reference: mfm.txt assembly l0060
      switch (p) {
        case 0:  // Short: data bit is 0, stay at CLOCK
          mfm_push_bit(m, 0);
          // state stays MFM_CLOCK
          break;
        case 1:  // Medium: data bit is 1, go to DATA
          mfm_push_bit(m, 1);
          m->state = MFM_DATA;
          break;
        case 2:  // Long: ERROR - missing clock violation
          // This shouldn't happen in valid data, reset
          mfm_reset(m);
          return false;
      }
      goto check_record;
  }

  return false;

check_record:
  // Check if we have the mark byte
  if (m->buf_pos == 1 && m->bytes_expected == 0) {
    uint8_t mark = m->buf[0];
    if (mark == 0xFE) {
      m->bytes_expected = 7;  // FE + track + side + sector + size + CRC(2)
    } else if (mark == 0xFB || mark == 0xFA) {
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

  // Check if record is complete
  if (m->bytes_expected > 0 && m->buf_pos >= m->bytes_expected) {
    uint8_t mark = m->buf[0];
    bool crc_ok = (m->crc == 0);

    if (mark == 0xFE) {
      if (crc_ok) {
        m->pending_track = m->buf[1];
        m->pending_side = m->buf[2];
        m->pending_sector = m->buf[3];
        // Clamp size_code to max 2 (512 bytes) to prevent buffer overflow
        uint8_t size_code = m->buf[4] & 0x03;
        m->pending_size_code = (size_code > 2) ? 2 : size_code;
        m->have_pending_addr = true;
      } else {
        m->crc_errors++;
        m->have_pending_addr = false;
      }
      mfm_reset(m);
    } else if ((mark == 0xFB || mark == 0xFA) && m->have_pending_addr) {
      uint16_t size = 128 << m->pending_size_code;
      out->track = m->pending_track;
      out->side = m->pending_side;
      out->sector_n = m->pending_sector;
      out->size_code = m->pending_size_code;
      out->size = size;
      out->valid = crc_ok;

      // Safety: ensure we don't read past buffer or write past output
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
