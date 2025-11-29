#include "mfm_encode.h"
#include "crc.h"
#include <string.h>

// ============== Internal helpers ==============

// Emit a pulse with given timing
static void mfm_encode_pulse(mfm_encode_t *e, uint8_t timing) {
    if (e->pos < e->size) {
        e->buf[e->pos++] = timing;
    }
}

// Emit pulse based on pending half-cells
static void mfm_encode_emit(mfm_encode_t *e) {
    if (e->pending_cells <= 1) {
        mfm_encode_pulse(e, MFM_PULSE_SHORT);
    } else if (e->pending_cells == 2) {
        mfm_encode_pulse(e, MFM_PULSE_MEDIUM);
    } else {
        mfm_encode_pulse(e, MFM_PULSE_LONG);
    }
    e->pending_cells = 0;
}

// ============== Public API ==============

void mfm_encode_init(mfm_encode_t *e, uint8_t *buf, size_t size) {
    e->buf = buf;
    e->size = size;
    e->pos = 0;
    e->prev_bit = 0;
    e->pending_cells = 0;
}

void mfm_encode_bytes(mfm_encode_t *e, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        for (int bit_idx = 7; bit_idx >= 0; bit_idx--) {
            int data_bit = (byte >> bit_idx) & 1;

            // Clock bit: 1 if prev=0 AND current=0
            int clock_bit = (e->prev_bit == 0 && data_bit == 0) ? 1 : 0;

            // Process clock half-cell
            if (clock_bit) {
                mfm_encode_emit(e);
            } else {
                e->pending_cells++;
            }

            // Process data half-cell
            if (data_bit) {
                mfm_encode_emit(e);
            } else {
                e->pending_cells++;
            }

            e->prev_bit = data_bit;
        }
    }
}

void mfm_encode_sync(mfm_encode_t *e) {
    // Preamble: 12 bytes of 0x00
    uint8_t preamble[12] = {0};
    mfm_encode_bytes(e, preamble, 12);

    // Sync marks: 3x 0xA1 with missing clock bit
    // Pattern: M L M L M S L M L M S L M L M
    // This violates normal MFM rules but is how sync is detected
    static const uint8_t sync_pulses[] = {
        MFM_PULSE_MEDIUM, MFM_PULSE_LONG, MFM_PULSE_MEDIUM, MFM_PULSE_LONG, MFM_PULSE_MEDIUM,
        MFM_PULSE_SHORT,
        MFM_PULSE_LONG, MFM_PULSE_MEDIUM, MFM_PULSE_LONG, MFM_PULSE_MEDIUM,
        MFM_PULSE_SHORT,
        MFM_PULSE_LONG, MFM_PULSE_MEDIUM, MFM_PULSE_LONG, MFM_PULSE_MEDIUM
    };

    for (int i = 0; i < 15; i++) {
        mfm_encode_pulse(e, sync_pulses[i]);
    }

    // After sync, we're at a data '1' bit position (last bit of 0xA1)
    e->prev_bit = 1;
    e->pending_cells = 0;
}

void mfm_encode_gap(mfm_encode_t *e, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint8_t gap = 0x4E;
        mfm_encode_bytes(e, &gap, 1);
    }
}

void mfm_encode_sector(mfm_encode_t *e, const sector_t *s) {
    // Address record: FE + track + side + sector + size_code + CRC
    uint8_t addr[5] = {0xFE, s->track, s->side, s->sector_n, 0x02};  // 0x02 = 512 bytes
    uint16_t addr_crc = crc16_mfm(addr, 5);

    mfm_encode_sync(e);
    mfm_encode_bytes(e, addr, 5);
    uint8_t addr_crc_bytes[2] = {addr_crc >> 8, addr_crc & 0xFF};
    mfm_encode_bytes(e, addr_crc_bytes, 2);

    // Gap 2 (between address and data)
    mfm_encode_gap(e, 22);

    // Data record: FB + 512 bytes + CRC
    uint8_t data_mark = 0xFB;
    uint16_t data_crc = 0xFFFF;
    data_crc = crc16_update(data_crc, 0xA1);
    data_crc = crc16_update(data_crc, 0xA1);
    data_crc = crc16_update(data_crc, 0xA1);
    data_crc = crc16_update(data_crc, data_mark);
    for (int i = 0; i < SECTOR_SIZE; i++) {
        data_crc = crc16_update(data_crc, s->data[i]);
    }

    mfm_encode_sync(e);
    mfm_encode_bytes(e, &data_mark, 1);
    mfm_encode_bytes(e, s->data, SECTOR_SIZE);
    uint8_t data_crc_bytes[2] = {data_crc >> 8, data_crc & 0xFF};
    mfm_encode_bytes(e, data_crc_bytes, 2);
}

size_t mfm_encode_track(mfm_encode_t *e, const track_t *t) {
    // Gap 4a (post-index gap)
    mfm_encode_gap(e, 80);

    // Encode all 18 sectors
    for (int i = 0; i < SECTORS_PER_TRACK; i++) {
        mfm_encode_sector(e, &t->sectors[i]);

        // Gap 3 (between sectors)
        mfm_encode_gap(e, 54);
    }

    // Fill remaining with gap bytes (Gap 4b)
    // A track at 300 RPM, 500kbps MFM is ~12500 bytes of flux data
    // We just return how much we wrote; caller can pad if needed

    return e->pos;
}
