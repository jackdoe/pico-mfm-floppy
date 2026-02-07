#include "mfm_encode.h"
#include "crc.h"
#include <string.h>

static void mfm_encode_pulse(mfm_encode_t *e, uint8_t timing) {
    if (e->pos < e->size) {
        e->buf[e->pos++] = timing;
    } else {
        e->overflow = true;
    }
}

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

void mfm_encode_init(mfm_encode_t *e, uint8_t *buf, size_t size) {
    e->buf = buf;
    e->size = size;
    e->pos = 0;
    e->prev_bit = 0;
    e->pending_cells = 0;
    e->overflow = false;
}

void mfm_encode_bytes(mfm_encode_t *e, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        for (int bit_idx = 7; bit_idx >= 0; bit_idx--) {
            int data_bit = (byte >> bit_idx) & 1;

            int clock_bit = (e->prev_bit == 0 && data_bit == 0) ? 1 : 0;

            if (clock_bit) {
                mfm_encode_emit(e);
            } else {
                e->pending_cells++;
            }

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
    uint8_t preamble[12] = {0};
    mfm_encode_bytes(e, preamble, 12);

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

    e->prev_bit = 1;
    e->pending_cells = 0;
}

void mfm_encode_gap(mfm_encode_t *e, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint8_t gap = MFM_GAP_BYTE;
        mfm_encode_bytes(e, &gap, 1);
    }
}

void mfm_encode_sector(mfm_encode_t *e, const sector_t *s) {
    uint8_t addr[5] = {MFM_ADDR_MARK, s->track, s->side, s->sector_n, 0x02};
    uint16_t addr_crc = crc16_mfm(addr, 5);

    mfm_encode_sync(e);
    mfm_encode_bytes(e, addr, 5);
    uint8_t addr_crc_bytes[2] = {addr_crc >> 8, addr_crc & 0xFF};
    mfm_encode_bytes(e, addr_crc_bytes, 2);

    mfm_encode_gap(e, 22);

    uint8_t data_mark = MFM_DATA_MARK;
    uint16_t data_crc = crc16(s->data, SECTOR_SIZE, crc16_mfm(&data_mark, 1));

    mfm_encode_sync(e);
    mfm_encode_bytes(e, &data_mark, 1);
    mfm_encode_bytes(e, s->data, SECTOR_SIZE);
    uint8_t data_crc_bytes[2] = {data_crc >> 8, data_crc & 0xFF};
    mfm_encode_bytes(e, data_crc_bytes, 2);
}

static void mfm_encode_precomp(uint8_t *buf, size_t len, uint8_t track) {
    if (len < 3) return;
    int shift = MFM_PRECOMP_SHIFT + (track - MFM_PRECOMP_START_TRACK) / 13;
    for (size_t i = 1; i < len - 1; i++) {
        if (buf[i] != MFM_PULSE_SHORT) continue;
        bool prev_long = (buf[i - 1] == MFM_PULSE_LONG);
        bool next_long = (buf[i + 1] == MFM_PULSE_LONG);
        if (prev_long && next_long) continue;
        if (prev_long) buf[i] -= shift;
        else if (next_long) buf[i] += shift;
    }
}

size_t mfm_encode_track(mfm_encode_t *e, const track_t *t) {
    mfm_encode_gap(e, 80);

    for (int i = 0; i < SECTORS_PER_TRACK; i++) {
        mfm_encode_sector(e, &t->sectors[i]);

        mfm_encode_gap(e, 54);
    }

    if (t->track >= MFM_PRECOMP_START_TRACK) {
        mfm_encode_precomp(e->buf, e->pos, t->track);
    }

    return e->pos;
}
