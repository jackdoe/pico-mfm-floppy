#ifndef MFM_DECODE_H
#define MFM_DECODE_H

#include <stdint.h>
#include <stdbool.h>
#include "floppy.h"

// ============== Configuration ==============
#define MFM_MIN_PREAMBLE 60

// ============== Types ==============
typedef enum { MFM_HUNT, MFM_SYNCING, MFM_DATA, MFM_CLOCK } mfm_state_t;

typedef struct {
  mfm_state_t state;

  // Pulse classification thresholds
  uint16_t T2_max;   // Max for Short (2T)
  uint16_t T3_max;   // Max for Medium (3T), above = Long

  // Preamble detection
  uint16_t short_count;

  // Sync detection
  uint8_t sync_stage;

  // Bit accumulation
  uint8_t byte_acc;
  uint8_t bit_count;

  // Record reading
  uint8_t buf[SECTOR_SIZE + 16];
  uint16_t buf_pos;
  uint16_t bytes_expected;

  // CRC
  uint16_t crc;

  // Pending address info
  uint8_t pending_track;
  uint8_t pending_side;
  uint8_t pending_sector;
  uint8_t pending_size_code;
  bool have_pending_addr;

  // Stats
  uint32_t syncs_found;
  uint32_t sectors_read;
  uint32_t crc_errors;
} mfm_t;

// ============== API ==============

// Initialize decoder state
void mfm_init(mfm_t *m);

// Reset decoder to hunt state
void mfm_reset(mfm_t *m);

// Feed a pulse delta time to the decoder
// Returns true if a complete sector was decoded (stored in out)
bool mfm_feed(mfm_t *m, uint16_t delta, sector_t *out);

// Print decoder statistics
void mfm_print_stats(mfm_t *m);

#endif // MFM_DECODE_H
