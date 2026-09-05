#ifndef MFM_H
#define MFM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "disk.h"

#define MFM_TICK_HZ 24000000u
#define MFM_READ_PIO_HZ (3u * MFM_TICK_HZ)
#define MFM_WRITE_PIO_HZ MFM_TICK_HZ
#define MFM_CELL_TICKS 48u
#define MFM_PULSE_SHORT (MFM_CELL_TICKS)
#define MFM_PULSE_MEDIUM (MFM_CELL_TICKS * 3u / 2u)
#define MFM_PULSE_LONG (MFM_CELL_TICKS * 2u)
#define MFM_PULSE_FLOOR (MFM_CELL_TICKS * 4u / 5u)
#define MFM_PULSE_CEILING (MFM_CELL_TICKS * 5u / 2u)
#define MFM_MIN_PREAMBLE 60u
#define MFM_ID_DATA_MAX_PULSES 512u
#define MFM_GAP_BYTE 0x4Eu
#define MFM_ADDR_MARK 0xFEu
#define MFM_DATA_MARK 0xFBu
#define MFM_DELETED_MARK 0xF8u
#define MFM_SIZE_CODE 2u
#define MFM_CRC_INIT 0xCDB4u
#define MFM_PRECOMP_SHIFT 3
#define MFM_PRECOMP_START_TRACK 40u

typedef enum {
  MFM_SHORT = 0,
  MFM_MEDIUM = 1,
  MFM_LONG = 2,
} mfm_pulse_t;

typedef bool (*mfm_emit_fn)(void *ctx, uint8_t pulse);

typedef struct {
  uint8_t *buf;
  size_t size;
  size_t pos;
  mfm_emit_fn emit;
  void *emit_ctx;
  int prev_bit;
  int pending_cells;
  bool overflow;
  bool stopped;
  int precomp_shift;
  uint8_t held;
  int8_t edge_shift;
  bool held_valid;
} mfm_encode_t;

void mfm_encode_init(mfm_encode_t *encoder, uint8_t *buf, size_t size);
void mfm_encode_init_emit(mfm_encode_t *encoder, mfm_emit_fn emit, void *ctx);
void mfm_encode_bytes(mfm_encode_t *encoder, const uint8_t *data, size_t len);
void mfm_encode_sync(mfm_encode_t *encoder);
void mfm_encode_gap(mfm_encode_t *encoder, size_t count);
void mfm_encode_sector(mfm_encode_t *encoder, uint8_t cylinder, uint8_t head,
                       uint8_t sector, const uint8_t data[DISK_SECTOR_SIZE]);
size_t mfm_encode_track(mfm_encode_t *encoder, const track_t *track);

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
  uint8_t byte_acc;
  uint8_t bit_count;
  uint16_t buf_pos;
  uint16_t bytes_expected;
  uint16_t crc;
  uint8_t sync_stage;
  uint32_t cell_q8;
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

static inline uint16_t mfm_short_limit(const mfm_t *decoder) {
  return (uint16_t)(decoder->cell_q8 * 5u / (4u * 256u));
}

static inline uint16_t mfm_medium_limit(const mfm_t *decoder) {
  return (uint16_t)(decoder->cell_q8 * 7u / (4u * 256u));
}

void mfm_init(mfm_t *decoder);
void mfm_reset(mfm_t *decoder);
bool mfm_feed(mfm_t *decoder, uint16_t delta, mfm_sector_t *out);

#endif
