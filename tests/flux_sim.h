#ifndef FLUX_SIM_H
#define FLUX_SIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
    uint8_t resolution;

    flux_rev_t rev;

    uint32_t jitter_seed;
    int16_t jitter_range;
    int32_t drift_ppm;
} flux_sim_t;

bool flux_sim_open_scp(flux_sim_t *sim, uint8_t *data, size_t size);
bool flux_sim_seek(flux_sim_t *sim, uint8_t track, uint8_t side, uint8_t rev);
bool flux_sim_next(flux_sim_t *sim, uint16_t *delta);
void flux_sim_close(flux_sim_t *sim);

void flux_sim_set_jitter(flux_sim_t *sim, int16_t range, uint32_t seed);
void flux_sim_set_drift(flux_sim_t *sim, int32_t ppm);

bool flux_sim_from_track(flux_sim_t *sim, const uint8_t *pulse_buf, size_t pulse_count);

uint8_t *scp_encode_disk(const uint8_t sectors[2880][512], size_t *out_size);

#endif
