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

#define MFM_PRECOMP_SHIFT 3
#define MFM_PRECOMP_START_TRACK 40

typedef struct {
    uint8_t *buf;
    size_t size;
    size_t pos;
    int prev_bit;
    int pending_cells;
    bool overflow;
} mfm_encode_t;

void mfm_encode_precomp(uint8_t *buf, size_t len);

void mfm_encode_init(mfm_encode_t *e, uint8_t *buf, size_t size);

void mfm_encode_bytes(mfm_encode_t *e, const uint8_t *data, size_t len);

void mfm_encode_sync(mfm_encode_t *e);

void mfm_encode_gap(mfm_encode_t *e, size_t count);

void mfm_encode_sector(mfm_encode_t *e, const sector_t *s);

size_t mfm_encode_track(mfm_encode_t *e, const track_t *t);

#endif
