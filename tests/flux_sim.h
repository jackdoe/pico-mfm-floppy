#ifndef FLUX_SIM_H
#define FLUX_SIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../src/block.h"
#include "flux_noise.h"

typedef struct {
    uint16_t *deltas;
    uint32_t count;
    uint32_t pos;
    uint32_t capacity;
} flux_rev_t;

typedef struct {
    uint8_t *file_data;
    size_t file_size;

    uint8_t start_track;
    uint8_t end_track;
    uint8_t num_revolutions;

    flux_rev_t rev;

    flux_noise_t noise;
} flux_sim_t;

bool flux_sim_open_scp(flux_sim_t *sim, uint8_t *data, size_t size);
bool flux_sim_seek(flux_sim_t *sim, uint8_t track, uint8_t side, uint8_t rev);
bool flux_sim_next(flux_sim_t *sim, uint16_t *delta);
void flux_sim_close(flux_sim_t *sim);

bool flux_sim_set_noise(flux_sim_t *sim, flux_noise_config_t config);

bool flux_sim_from_track(flux_sim_t *sim, const uint8_t *pulse_buf, size_t pulse_count);

uint8_t *scp_encode_disk(const uint8_t sectors[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE],
                         size_t *out_size);

#endif
