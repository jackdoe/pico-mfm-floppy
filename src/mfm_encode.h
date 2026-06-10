#ifndef MFM_ENCODE_H
#define MFM_ENCODE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "floppy.h"

#define MFM_PIO_OVERHEAD 19
#define MFM_PULSE_SHORT  (48 - MFM_PIO_OVERHEAD)
#define MFM_PULSE_MEDIUM (72 - MFM_PIO_OVERHEAD)
#define MFM_PULSE_LONG   (96 - MFM_PIO_OVERHEAD)

#define MFM_GAP_BYTE 0x4E
#define MFM_ADDR_MARK 0xFE
#define MFM_DATA_MARK 0xFB
#define MFM_DELETED_MARK 0xFA

#define MFM_PRECOMP_SHIFT 3
#define MFM_PRECOMP_START_TRACK 40

typedef void (*mfm_emit_fn)(void *ctx, uint8_t pulse);

typedef struct {
    uint8_t *buf;
    size_t size;
    size_t pos;
    mfm_emit_fn emit;
    void *emit_ctx;
    int prev_bit;
    int pending_cells;
    bool overflow;
    int precomp_shift;
    uint8_t held;
    uint8_t last_out;
    bool held_valid;
    bool held_first;
} mfm_encode_t;

void mfm_encode_init(mfm_encode_t *e, uint8_t *buf, size_t size);

void mfm_encode_init_emit(mfm_encode_t *e, mfm_emit_fn emit, void *ctx);

void mfm_encode_bytes(mfm_encode_t *e, const uint8_t *data, size_t len);

void mfm_encode_sync(mfm_encode_t *e);

void mfm_encode_gap(mfm_encode_t *e, size_t count);

void mfm_encode_sector(mfm_encode_t *e, const sector_t *s);

size_t mfm_encode_track(mfm_encode_t *e, const track_t *t);

#endif
