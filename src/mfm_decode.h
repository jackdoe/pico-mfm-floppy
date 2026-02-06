#ifndef MFM_DECODE_H
#define MFM_DECODE_H

#include <stdint.h>
#include <stdbool.h>
#include "floppy.h"

#define MFM_MIN_PREAMBLE 60

typedef enum { MFM_HUNT, MFM_SYNCING, MFM_DATA, MFM_CLOCK } mfm_state_t;

typedef struct {
  mfm_state_t state;
  uint16_t T2_max;
  uint16_t T3_max;
  uint8_t byte_acc;
  uint8_t bit_count;
  uint16_t buf_pos;
  uint16_t bytes_expected;
  uint16_t crc;
  bool overflow;
  uint8_t sync_stage;

  uint16_t short_count;
  uint32_t preamble_sum;

  uint8_t pending_track;
  uint8_t pending_side;
  uint8_t pending_sector;
  uint8_t pending_size_code;
  bool have_pending_addr;

  uint32_t syncs_found;
  uint32_t sectors_read;
  uint32_t crc_errors;

  uint8_t buf[SECTOR_SIZE + 16];
} mfm_t;

void mfm_init(mfm_t *m);

void mfm_reset(mfm_t *m);

bool mfm_feed(mfm_t *m, uint16_t delta, sector_t *out);

void mfm_print_stats(mfm_t *m);

#endif
