#ifndef MFM_DECODE_H
#define MFM_DECODE_H

#include <stdbool.h>
#include <stdint.h>
#include "block.h"

#define MFM_MIN_PREAMBLE 60u
#define MFM_PULSE_FLOOR 38u
#define MFM_PULSE_CEILING 120u
#define MFM_ID_DATA_MAX_PULSES 512u

typedef enum {
  MFM_SHORT = 0,
  MFM_MEDIUM = 1,
  MFM_LONG = 2,
} mfm_pulse_t;

typedef enum {
  MFM_HUNT = 0,
  MFM_SYNCING,
  MFM_DATA,
  MFM_CLOCK,
} mfm_state_t;

typedef enum {
  MFM_EXPECT_ID = 0,
  MFM_EXPECT_DATA,
  MFM_READING_DATA,
} mfm_record_state_t;

typedef struct {
  uint8_t cylinder;
  uint8_t head;
  uint8_t sector;
  uint8_t data[DISK_SECTOR_SIZE];
} mfm_sector_t;

typedef struct {
  mfm_state_t state;
  mfm_record_state_t record_state;
  uint16_t T2_max;
  uint16_t T3_max;
  uint8_t byte_acc;
  uint8_t bit_count;
  uint16_t buf_pos;
  uint16_t bytes_expected;
  uint16_t crc;
  uint8_t sync_stage;
  uint16_t t_cell;
  uint16_t short_count;
  uint32_t preamble_sum;
  uint8_t pending_cylinder;
  uint8_t pending_head;
  uint8_t pending_sector;
  uint16_t pending_pulses;
  uint32_t syncs_found;
  uint32_t sectors_read;
  uint32_t crc_errors;
  uint32_t format_errors;
  uint8_t buf[DISK_SECTOR_SIZE + 3u];
} mfm_t;

void mfm_init(mfm_t *decoder);
void mfm_reset(mfm_t *decoder);
bool mfm_feed(mfm_t *decoder, uint16_t delta, mfm_sector_t *out);

#endif
