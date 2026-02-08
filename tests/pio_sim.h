#ifndef PIO_SIM_H
#define PIO_SIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PIO_SIM_MAX_FLUX 200000

typedef struct {
    uint16_t *deltas;
    uint32_t count;
} pio_sim_track_t;

typedef struct {
    pio_sim_track_t tracks[80][2];

    uint8_t head_track;
    uint8_t head_side;
    bool motor_on;
    bool selected;
    bool write_protected;
    bool step_direction_inward;

    uint16_t *read_buf;
    uint32_t read_count;
    uint32_t read_pos;
    uint16_t counter;
    bool index_state;
    uint32_t flux_in_rev;

    uint8_t *write_capture;
    uint32_t write_capture_count;
    uint32_t write_capture_capacity;

    uint32_t index_poll_count;
    int fault_writes_remaining;
} pio_sim_drive_t;

void pio_sim_init(pio_sim_drive_t *drive);
void pio_sim_free(pio_sim_drive_t *drive);
bool pio_sim_load_scp(pio_sim_drive_t *drive, uint8_t *scp_data, size_t scp_size);
void pio_sim_install(pio_sim_drive_t *drive);

#endif
