#include "flux_sim.h"
#include "../src/mfm_encode.h"
#include <stdlib.h>
#include <string.h>

static uint32_t read_le32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_be16(const uint8_t *p) {
    return (p[0] << 8) | p[1];
}

static void flux_rev_ensure(flux_rev_t *rev, uint32_t needed) {
    if (rev->capacity >= needed) return;
    uint32_t cap = needed + needed / 4;
    rev->deltas = realloc(rev->deltas, cap * sizeof(uint16_t));
    rev->capacity = cap;
}

static int16_t flux_sim_jitter(flux_sim_t *sim) {
    if (sim->jitter_range == 0) return 0;
    sim->jitter_seed = sim->jitter_seed * 1103515245 + 12345;
    int32_t r = (sim->jitter_seed >> 16) & 0x7FFF;
    return (r % (2 * sim->jitter_range + 1)) - sim->jitter_range;
}

bool flux_sim_open_scp(flux_sim_t *sim, uint8_t *data, size_t size) {
    memset(sim, 0, sizeof(*sim));
    if (size < 0x10) return false;
    if (data[0] != 'S' || data[1] != 'C' || data[2] != 'P') return false;

    sim->file_data = data;
    sim->file_size = size;
    sim->num_revolutions = data[5];
    sim->start_track = data[6];
    sim->end_track = data[7];
    sim->resolution = data[9];

    return true;
}

bool flux_sim_seek(flux_sim_t *sim, uint8_t track, uint8_t side, uint8_t rev) {
    if (!sim->file_data) return false;
    if (rev >= sim->num_revolutions) return false;

    uint16_t scp_idx = track * 2 + side;
    uint32_t table_off = 0x10 + scp_idx * 4;
    if (table_off + 4 > sim->file_size) return false;

    uint32_t tdh_off = read_le32(sim->file_data + table_off);
    if (tdh_off == 0) return false;
    if (tdh_off + 4 + (rev + 1) * 12 > sim->file_size) return false;

    uint8_t *rev_entry = sim->file_data + tdh_off + 4 + rev * 12;
    uint32_t flux_count = read_le32(rev_entry + 4);
    uint32_t data_off = read_le32(rev_entry + 8);

    uint8_t *flux_data = sim->file_data + tdh_off + data_off;
    if (tdh_off + data_off + flux_count * 2 > sim->file_size) return false;

    flux_rev_ensure(&sim->rev, flux_count);

    uint32_t out_pos = 0;
    uint32_t accumulator = 0;
    uint32_t scale_num = (sim->resolution + 1) * 3;

    for (uint32_t i = 0; i < flux_count; i++) {
        uint16_t scp_val = read_be16(flux_data + i * 2);
        if (scp_val == 0) {
            accumulator += 65536;
            continue;
        }
        uint32_t total = accumulator + scp_val;
        accumulator = 0;

        uint32_t our_delta = (total * scale_num + 2) / 5;
        if (our_delta > 0xFFFF) our_delta = 0xFFFF;
        sim->rev.deltas[out_pos++] = (uint16_t)our_delta;
    }

    sim->rev.count = out_pos;
    sim->rev.pos = 0;
    return true;
}

bool flux_sim_next(flux_sim_t *sim, uint16_t *delta) {
    if (sim->rev.pos >= sim->rev.count) return false;

    int32_t d = sim->rev.deltas[sim->rev.pos++];

    if (sim->drift_ppm != 0) {
        d = (d * (1000000 + sim->drift_ppm)) / 1000000;
    }

    d += flux_sim_jitter(sim);

    if (d < 1) d = 1;
    if (d > 0xFFFF) d = 0xFFFF;

    *delta = (uint16_t)d;
    return true;
}

void flux_sim_close(flux_sim_t *sim) {
    free(sim->rev.deltas);
    memset(sim, 0, sizeof(*sim));
}

void flux_sim_set_jitter(flux_sim_t *sim, int16_t range, uint32_t seed) {
    sim->jitter_range = range;
    sim->jitter_seed = seed;
}

void flux_sim_set_drift(flux_sim_t *sim, int32_t ppm) {
    sim->drift_ppm = ppm;
}

bool flux_sim_from_track(flux_sim_t *sim, const uint8_t *pulse_buf, size_t pulse_count) {
    memset(sim, 0, sizeof(*sim));
    flux_rev_ensure(&sim->rev, pulse_count);

    for (size_t i = 0; i < pulse_count; i++) {
        sim->rev.deltas[i] = pulse_buf[i] + MFM_PIO_OVERHEAD;
    }

    sim->rev.count = pulse_count;
    sim->rev.pos = 0;
    return true;
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static void write_be16(uint8_t *p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}

#define SCP_NUM_TRACKS 160
#define SCP_HEADER_SIZE 16
#define SCP_TABLE_SIZE (SCP_NUM_TRACKS * 4)
#define SCP_TDH_SIZE (4 + 12)

uint8_t *scp_encode_disk(const uint8_t sectors[2880][512], size_t *out_size) {
    static uint8_t encode_buf[200000];

    size_t max_file = SCP_HEADER_SIZE + SCP_TABLE_SIZE +
                      SCP_NUM_TRACKS * (SCP_TDH_SIZE + 200000);
    uint8_t *file = malloc(max_file);
    if (!file) return NULL;

    memset(file, 0, SCP_HEADER_SIZE + SCP_TABLE_SIZE);
    file[0] = 'S'; file[1] = 'C'; file[2] = 'P';
    file[3] = 0;
    file[4] = 0x80;
    file[5] = 1;
    file[6] = 0;
    file[7] = SCP_NUM_TRACKS - 1;
    file[8] = 0x01;
    file[9] = 0;
    file[10] = 0;

    size_t write_pos = SCP_HEADER_SIZE + SCP_TABLE_SIZE;

    for (int trk = 0; trk < 80; trk++) {
        for (int side = 0; side < 2; side++) {
            int scp_idx = trk * 2 + side;

            track_t t;
            memset(&t, 0, sizeof(t));
            t.track = trk;
            t.side = side;
            for (int s = 0; s < SECTORS_PER_TRACK; s++) {
                int lba = (trk * 2 + side) * SECTORS_PER_TRACK + s;
                t.sectors[s].track = trk;
                t.sectors[s].side = side;
                t.sectors[s].sector_n = s + 1;
                t.sectors[s].size_code = 2;
                t.sectors[s].valid = true;
                memcpy(t.sectors[s].data, sectors[lba], SECTOR_SIZE);
            }

            mfm_encode_t enc;
            mfm_encode_init(&enc, encode_buf, sizeof(encode_buf));
            mfm_encode_track(&enc, &t);

            uint32_t flux_count = enc.pos;
            uint32_t duration = 0;
            for (size_t i = 0; i < enc.pos; i++) {
                duration += (encode_buf[i] + MFM_PIO_OVERHEAD) * 5 / 3;
            }

            write_le32(file + SCP_HEADER_SIZE + scp_idx * 4, write_pos);

            uint8_t *tdh = file + write_pos;
            tdh[0] = 'T'; tdh[1] = 'R'; tdh[2] = 'K';
            tdh[3] = scp_idx;
            write_le32(tdh + 4, duration);
            write_le32(tdh + 8, flux_count);
            write_le32(tdh + 12, SCP_TDH_SIZE);

            uint8_t *flux_out = tdh + SCP_TDH_SIZE;
            for (size_t i = 0; i < enc.pos; i++) {
                uint32_t scp_val = (encode_buf[i] + MFM_PIO_OVERHEAD) * 5 / 3;
                if (scp_val > 0xFFFF) scp_val = 0xFFFF;
                write_be16(flux_out + i * 2, (uint16_t)scp_val);
            }

            write_pos += SCP_TDH_SIZE + flux_count * 2;
        }
    }

    *out_size = write_pos;
    return file;
}
