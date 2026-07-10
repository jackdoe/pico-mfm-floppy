#ifndef MFM_ENCODE_H
#define MFM_ENCODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "block.h"

#define MFM_PIO_OVERHEAD 19u
#define MFM_PULSE_SHORT (48u - MFM_PIO_OVERHEAD)
#define MFM_PULSE_MEDIUM (72u - MFM_PIO_OVERHEAD)
#define MFM_PULSE_LONG (96u - MFM_PIO_OVERHEAD)
#define MFM_GAP_BYTE 0x4Eu
#define MFM_ADDR_MARK 0xFEu
#define MFM_DATA_MARK 0xFBu
#define MFM_DELETED_MARK 0xF8u
#define MFM_SIZE_CODE 2u
#define MFM_PRECOMP_SHIFT 3
#define MFM_PRECOMP_START_TRACK 40u

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
  uint8_t last_out;
  bool held_valid;
  bool held_first;
} mfm_encode_t;

void mfm_encode_init(mfm_encode_t *encoder, uint8_t *buf, size_t size);
void mfm_encode_init_emit(mfm_encode_t *encoder, mfm_emit_fn emit, void *ctx);
void mfm_encode_bytes(mfm_encode_t *encoder, const uint8_t *data, size_t len);
void mfm_encode_sync(mfm_encode_t *encoder);
void mfm_encode_gap(mfm_encode_t *encoder, size_t count);
void mfm_encode_sector(mfm_encode_t *encoder, uint8_t cylinder, uint8_t head,
                       uint8_t sector, const uint8_t data[DISK_SECTOR_SIZE]);
size_t mfm_encode_track(mfm_encode_t *encoder, const track_t *track);

#endif
