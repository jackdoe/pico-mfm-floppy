#ifndef MFM_ENCODE_H
#define MFM_ENCODE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "floppy.h"

// ============== Configuration ==============
// Pulse counts in PIO clock units (at 24MHz, 1 count = 41.67ns)
// For MFM HD: 2T=2us, 3T=3us, 4T=4us
// At 24MHz: 2us=48 counts, 3us=72 counts, 4us=96 counts
//
// PIO overhead is 19 cycles, so we subtract that from the total time
// to get the wait loop count. The values below are what gets sent to PIO.
#define MFM_PIO_OVERHEAD 19
#define MFM_PULSE_SHORT  (48 - MFM_PIO_OVERHEAD)   // 2T (~2us) -> 29
#define MFM_PULSE_MEDIUM (72 - MFM_PIO_OVERHEAD)   // 3T (~3us) -> 53
#define MFM_PULSE_LONG   (96 - MFM_PIO_OVERHEAD)   // 4T (~4us) -> 77

// ============== Encoder State ==============
typedef struct {
    uint8_t *buf;        // Output buffer for pulse timings
    size_t size;         // Buffer capacity
    size_t pos;          // Current write position
    int prev_bit;        // Last data bit (for clock generation)
    int pending_cells;   // Half-cells since last transition
} mfm_encode_t;

// ============== API ==============

// Initialize encoder state
void mfm_encode_init(mfm_encode_t *e, uint8_t *buf, size_t size);

// Encode data bytes to MFM pulses
void mfm_encode_bytes(mfm_encode_t *e, const uint8_t *data, size_t len);

// Encode sync pattern: 12x 0x00 preamble + 3x 0xA1 with missing clock
void mfm_encode_sync(mfm_encode_t *e);

// Encode gap bytes (0x4E)
void mfm_encode_gap(mfm_encode_t *e, size_t count);

// Encode a complete sector (address mark + data mark)
void mfm_encode_sector(mfm_encode_t *e, const sector_t *s);

// Encode a complete track (18 sectors for HD 3.5")
// Returns number of bytes written to buffer
size_t mfm_encode_track(mfm_encode_t *e, const track_t *t);

#endif // MFM_ENCODE_H
