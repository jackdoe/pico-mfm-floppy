#include "flux_sim.h"
#include "../src/mfm_encode.h"
#include <stdlib.h>
#include <string.h>

#define SCP_TABLE_TRACKS 168u
#define SCP_DISK_TRACKS DISK_TRACK_COUNT
#define SCP_HEADER_SIZE 16u
#define SCP_TABLE_SIZE (SCP_TABLE_TRACKS * 4u)
#define SCP_TDH_SIZE 16u

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static bool flux_rev_ensure(flux_rev_t *rev, uint32_t needed) {
    if (needed == 0) return true;
    if (rev->capacity >= needed) return true;
    uint64_t grown = (uint64_t)needed + needed / 4u;
    if (grown > UINT32_MAX || grown * sizeof(uint16_t) > SIZE_MAX) return false;
    uint32_t cap = (uint32_t)grown;
    uint16_t *deltas = realloc(rev->deltas, (size_t)cap * sizeof(uint16_t));
    if (!deltas) return false;
    rev->deltas = deltas;
    rev->capacity = cap;
    return true;
}

bool flux_sim_open_scp(flux_sim_t *sim, uint8_t *data, size_t size) {
    if (!sim || !data) return false;
    memset(sim, 0, sizeof(*sim));
    if (size < 0x2B0) return false;
    if (data[0] != 'S' || data[1] != 'C' || data[2] != 'P') return false;
    if (data[5] == 0 || data[6] > data[7] || data[7] >= SCP_TABLE_TRACKS) return false;
    if (((data[8] & 1u) == 0 && data[5] != 1u) || data[9] != 0 ||
        data[10] != 0 || data[11] != 0) return false;
    uint32_t checksum = 0;
    for (size_t offset = 0x10; offset < size; offset++) checksum += data[offset];
    if (checksum != read_le32(data + 12)) return false;

    sim->file_data = data;
    sim->file_size = size;
    sim->num_revolutions = data[5];
    sim->start_track = data[6];
    sim->end_track = data[7];

    return true;
}

bool flux_sim_seek(flux_sim_t *sim, uint8_t track, uint8_t side, uint8_t rev) {
    if (!sim || !sim->file_data) return false;
    sim->rev.count = 0;
    sim->rev.pos = 0;
    if (rev >= sim->num_revolutions) return false;
    if (!disk_ch_valid(track, side)) return false;

    uint16_t scp_idx = track * 2 + side;
    if (scp_idx < sim->start_track || scp_idx > sim->end_track) return false;
    uint64_t table_off = 0x10 + (uint64_t)scp_idx * 4;
    if (table_off + 4 > sim->file_size) return false;

    uint64_t tdh_off = read_le32(sim->file_data + table_off);
    if (tdh_off < 0x2B0) return false;
    if (tdh_off + 4 + ((uint64_t)rev + 1) * 12 > sim->file_size) return false;
    if (sim->file_data[tdh_off] != 'T' || sim->file_data[tdh_off + 1] != 'R' ||
        sim->file_data[tdh_off + 2] != 'K' ||
        sim->file_data[tdh_off + 3] != scp_idx) return false;

    uint8_t *rev_entry = sim->file_data + tdh_off + 4 + rev * 12;
    uint32_t flux_count = read_le32(rev_entry + 4);
    uint64_t data_off = read_le32(rev_entry + 8);
    if (data_off < 4u + (uint64_t)sim->num_revolutions * 12u) return false;
    if (tdh_off + data_off + (uint64_t)flux_count * 2 > sim->file_size) return false;
    uint8_t *flux_data = sim->file_data + tdh_off + data_off;

    if (!flux_rev_ensure(&sim->rev, flux_count)) return false;

    uint32_t out_pos = 0;
    uint64_t accumulator = 0;
    uint32_t scale_num = 3;

    for (uint32_t i = 0; i < flux_count; i++) {
        uint16_t scp_val = read_be16(flux_data + i * 2);
        if (scp_val == 0) {
            accumulator += 65536;
            if (accumulator > UINT32_MAX) return false;
            continue;
        }
        uint64_t total = accumulator + scp_val;
        accumulator = 0;

        uint64_t our_delta = (total * scale_num + 2) / 5;
        if (our_delta > 0xFFFF) our_delta = 0xFFFF;
        sim->rev.deltas[out_pos++] = (uint16_t)our_delta;
    }
    if (accumulator != 0) return false;

    sim->rev.count = out_pos;
    sim->rev.pos = 0;
    return true;
}

bool flux_sim_next(flux_sim_t *sim, uint16_t *delta) {
    if (!sim || !delta) return false;
    if (sim->rev.pos >= sim->rev.count) return false;
    *delta = flux_noise_apply(&sim->noise, sim->rev.deltas[sim->rev.pos++]);
    return true;
}

void flux_sim_close(flux_sim_t *sim) {
    if (!sim) return;
    free(sim->rev.deltas);
    memset(sim, 0, sizeof(*sim));
}

bool flux_sim_set_noise(flux_sim_t *sim, flux_noise_config_t config) {
    return sim && flux_noise_configure(&sim->noise, &config);
}

bool flux_sim_from_track(flux_sim_t *sim, const uint8_t *pulse_buf, size_t pulse_count) {
    if (!sim || (!pulse_buf && pulse_count != 0) || pulse_count > UINT32_MAX) return false;
    memset(sim, 0, sizeof(*sim));
    if (!flux_rev_ensure(&sim->rev, (uint32_t)pulse_count)) return false;

    for (size_t i = 0; i < pulse_count; i++) {
        sim->rev.deltas[i] = pulse_buf[i] + MFM_PIO_OVERHEAD;
    }

    sim->rev.count = (uint32_t)pulse_count;
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

uint8_t *scp_encode_disk(
    const uint8_t sectors[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE], size_t *out_size) {
    if (!sectors || !out_size) return NULL;
    *out_size = 0;

    uint8_t *encode_buf = malloc(200000u);
    if (!encode_buf) return NULL;

    size_t max_file = SCP_HEADER_SIZE + SCP_TABLE_SIZE +
                      SCP_DISK_TRACKS * (SCP_TDH_SIZE + 400000u);
    uint8_t *file = malloc(max_file);
    if (!file) {
        free(encode_buf);
        return NULL;
    }

    memset(file, 0, SCP_HEADER_SIZE + SCP_TABLE_SIZE);
    file[0] = 'S'; file[1] = 'C'; file[2] = 'P';
    file[3] = 0;
    file[4] = 0x33;
    file[5] = 1;
    file[6] = 0;
    file[7] = SCP_DISK_TRACKS - 1;
    file[8] = 0x03;
    file[9] = 0;
    file[10] = 0;

    size_t write_pos = SCP_HEADER_SIZE + SCP_TABLE_SIZE;

    for (uint8_t trk = 0; trk < DISK_CYLINDERS; trk++) {
        for (uint8_t side = 0; side < DISK_HEADS; side++) {
            uint16_t scp_idx;
            if (!disk_ch_to_track(trk, side, &scp_idx)) {
                free(encode_buf);
                free(file);
                return NULL;
            }

            track_t t;
            memset(&t, 0, sizeof(t));
            t.cylinder = trk;
            t.head = side;
            t.valid = DISK_TRACK_VALID;
            for (uint8_t s = 0; s < DISK_SECTORS_PER_TRACK; s++) {
                uint16_t lba;
                if (!disk_chs_to_lba(trk, side, s, &lba)) {
                    free(encode_buf);
                    free(file);
                    return NULL;
                }
                memcpy(t.data[s], sectors[lba], DISK_SECTOR_SIZE);
            }

            mfm_encode_t enc;
            mfm_encode_init(&enc, encode_buf, 200000u);
            mfm_encode_track(&enc, &t);
            if (enc.overflow || enc.stopped) {
                free(encode_buf);
                free(file);
                return NULL;
            }

            uint32_t flux_count = (uint32_t)enc.pos;
            uint32_t duration = 0;
            for (size_t i = 0; i < enc.pos; i++) {
                duration += (encode_buf[i] + MFM_PIO_OVERHEAD) * 5 / 3;
            }

            if (write_pos > UINT32_MAX) {
                free(encode_buf);
                free(file);
                return NULL;
            }
            write_le32(file + SCP_HEADER_SIZE + scp_idx * 4,
                       (uint32_t)write_pos);

            uint8_t *tdh = file + write_pos;
            tdh[0] = 'T'; tdh[1] = 'R'; tdh[2] = 'K';
            tdh[3] = (uint8_t)scp_idx;
            write_le32(tdh + 4, duration);
            write_le32(tdh + 8, flux_count);
            write_le32(tdh + 12, SCP_TDH_SIZE);

            uint8_t *flux_out = tdh + SCP_TDH_SIZE;
            if (write_pos + SCP_TDH_SIZE + (size_t)flux_count * 2 > max_file) {
                free(encode_buf);
                free(file);
                return NULL;
            }
            for (size_t i = 0; i < enc.pos; i++) {
                uint32_t scp_val = (encode_buf[i] + MFM_PIO_OVERHEAD) * 5 / 3;
                if (scp_val > 0xFFFF) scp_val = 0xFFFF;
                write_be16(flux_out + i * 2, (uint16_t)scp_val);
            }

            write_pos += SCP_TDH_SIZE + flux_count * 2;
        }
    }

    uint32_t checksum = 0;
    for (size_t offset = SCP_HEADER_SIZE; offset < write_pos; offset++) {
        checksum += file[offset];
    }
    write_le32(file + 12, checksum);
    *out_size = write_pos;
    free(encode_buf);
    return file;
}
