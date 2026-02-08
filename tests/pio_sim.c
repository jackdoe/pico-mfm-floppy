#include "pio_sim.h"
#include "flux_sim.h"
#include "../src/floppy.h"
#include "../src/mfm_encode.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static pio_sim_drive_t *g_drive = NULL;
extern floppy_t *pio_sim_floppy_ref;

static uint32_t read_le32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_be16(const uint8_t *p) {
    return (p[0] << 8) | p[1];
}

void pio_sim_init(pio_sim_drive_t *drive) {
    memset(drive, 0, sizeof(*drive));
}

void pio_sim_free(pio_sim_drive_t *drive) {
    for (int t = 0; t < 80; t++) {
        for (int s = 0; s < 2; s++) {
            free(drive->tracks[t][s].deltas);
        }
    }
    free(drive->write_capture);
    memset(drive, 0, sizeof(*drive));
}

bool pio_sim_load_scp(pio_sim_drive_t *drive, uint8_t *data, size_t size) {
    if (size < 0x10 || data[0] != 'S' || data[1] != 'C' || data[2] != 'P')
        return false;

    uint8_t num_revs = data[5];
    uint8_t resolution = data[9];
    uint32_t scale_num = (resolution + 1) * 3;

    for (int track = 0; track < 80; track++) {
        for (int side = 0; side < 2; side++) {
            uint16_t scp_idx = track * 2 + side;
            uint32_t table_off = 0x10 + scp_idx * 4;
            if (table_off + 4 > size) continue;

            uint32_t tdh_off = read_le32(data + table_off);
            if (tdh_off == 0 || tdh_off + 4 + 12 > size) continue;

            uint8_t *rev_entry = data + tdh_off + 4;
            uint32_t flux_count = read_le32(rev_entry + 4);
            uint32_t data_off = read_le32(rev_entry + 8);

            if (tdh_off + data_off + flux_count * 2 > size) continue;

            uint8_t *flux_data = data + tdh_off + data_off;

            drive->tracks[track][side].deltas = malloc(flux_count * sizeof(uint16_t));
            if (!drive->tracks[track][side].deltas) continue;

            uint32_t out = 0;
            uint32_t acc = 0;
            for (uint32_t i = 0; i < flux_count; i++) {
                uint16_t val = read_be16(flux_data + i * 2);
                if (val == 0) { acc += 65536; continue; }
                uint32_t total = acc + val;
                acc = 0;
                uint32_t d = (total * scale_num + 2) / 5;
                if (d > 0xFFFF) d = 0xFFFF;
                drive->tracks[track][side].deltas[out++] = (uint16_t)d;
            }
            drive->tracks[track][side].count = out;

            (void)num_revs;
        }
    }

    return true;
}

void pio_sim_install(pio_sim_drive_t *drive) {
    g_drive = drive;
}

static void pio_sim_load_track(void) {
    if (!g_drive) return;
    pio_sim_track_t *t = &g_drive->tracks[g_drive->head_track][g_drive->head_side];
    g_drive->read_buf = t->deltas;
    g_drive->read_count = t->count;
    g_drive->read_pos = 0;
    g_drive->counter = 0;
    g_drive->index_state = false;
    g_drive->flux_in_rev = 0;
}

void gpio_init(uint pin) { (void)pin; }
void gpio_set_dir(uint pin, bool out) {
    if (!g_drive || !pio_sim_floppy_ref) return;
    floppy_t *f = pio_sim_floppy_ref;

    bool going_low = out;

    if (pin == f->pins.direction) {
        g_drive->step_direction_inward = going_low;
    } else if (pin == f->pins.step && going_low) {
        if (g_drive->step_direction_inward && g_drive->head_track < 79) {
            g_drive->head_track++;
        } else if (!g_drive->step_direction_inward && g_drive->head_track > 0) {
            g_drive->head_track--;
        }
        pio_sim_load_track();
    } else if (pin == f->pins.side_select) {
        uint8_t new_side = going_low ? 1 : 0;
        if (new_side != g_drive->head_side) {
            g_drive->head_side = new_side;
            pio_sim_load_track();
        }
    } else if (pin == f->pins.write_gate) {
        if (going_low) {
            g_drive->write_capture_count = 0;
        } else if (g_drive->write_capture_count > 0) {
            if (g_drive->fault_writes_remaining > 0) {
                g_drive->fault_writes_remaining--;
            } else {
                pio_sim_track_t *t = &g_drive->tracks[g_drive->head_track][g_drive->head_side];
                free(t->deltas);
                t->deltas = malloc(g_drive->write_capture_count * sizeof(uint16_t));
                t->count = g_drive->write_capture_count;
                for (uint32_t i = 0; i < g_drive->write_capture_count; i++) {
                    t->deltas[i] = g_drive->write_capture[i] + MFM_PIO_OVERHEAD;
                }
                pio_sim_load_track();
            }
        }
    }
}
void gpio_pull_up(uint pin) { (void)pin; }

void gpio_put(uint pin, bool value) {
    (void)pin; (void)value;
}

bool gpio_get(uint pin) {
    if (!g_drive || !pio_sim_floppy_ref) return true;
    floppy_t *f = pio_sim_floppy_ref;

    if (pin == f->pins.track0) {
        return g_drive->head_track != 0;
    }
    if (pin == f->pins.index) {
        g_drive->index_poll_count++;
        return (g_drive->index_poll_count & 0x100) != 0;
    }
    if (pin == f->pins.disk_change) {
        return true;
    }
    if (pin == f->pins.write_protect) {
        return !g_drive->write_protected;
    }
    return true;
}

uint pio_add_program(PIO pio, const pio_program_t *program) {
    (void)pio; (void)program;
    return 0;
}

uint pio_claim_unused_sm(PIO pio, bool required) {
    (void)pio; (void)required;
    return 0;
}

void pio_sm_init(PIO pio, uint sm, uint offset, const pio_sm_config *config) {
    (void)pio; (void)sm; (void)offset; (void)config;
}

void pio_sm_exec(PIO pio, uint sm, uint instr) {
    (void)pio; (void)sm; (void)instr;
}

void pio_sm_restart(PIO pio, uint sm) { (void)pio; (void)sm; }

void pio_sm_clear_fifos(PIO pio, uint sm) {
    (void)pio; (void)sm;
    if (g_drive) {
        pio_sim_load_track();
    }
}

void pio_sm_set_enabled(PIO pio, uint sm, bool enabled) {
    (void)pio; (void)sm; (void)enabled;
}

void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint32_t values, uint32_t mask) {
    (void)pio; (void)sm; (void)values; (void)mask;
}

void pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin, uint count, bool is_out) {
    (void)pio; (void)sm; (void)pin; (void)count; (void)is_out;
}

void pio_gpio_init(PIO pio, uint pin) { (void)pio; (void)pin; }

bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm) {
    (void)pio; (void)sm;
    if (!g_drive || !g_drive->read_buf) return true;
    return g_drive->read_count == 0;
}

static uint16_t pio_sim_next_sample(void) {
    if (!g_drive || !g_drive->read_buf || g_drive->read_count == 0)
        return (0x7FFF << 1) | 1;

    if (g_drive->read_pos >= g_drive->read_count)
        g_drive->read_pos = 0;

    uint16_t delta = g_drive->read_buf[g_drive->read_pos++];
    g_drive->counter -= delta;

    g_drive->flux_in_rev++;

    bool idx = false;
    if (g_drive->flux_in_rev >= g_drive->read_count) {
        idx = true;
        g_drive->flux_in_rev = 0;
        g_drive->index_state = !g_drive->index_state;
    }

    return ((uint16_t)(g_drive->counter & 0x7FFF) << 1) | (idx ? 0 : 1);
}

uint32_t pio_sm_get_blocking(PIO pio, uint sm) {
    (void)pio; (void)sm;
    uint16_t lo = pio_sim_next_sample();
    uint16_t hi = pio_sim_next_sample();
    return ((uint32_t)hi << 16) | lo;
}

bool pio_sm_is_tx_fifo_empty(PIO pio, uint sm) {
    (void)pio; (void)sm;
    return true;
}

void pio_sm_put_blocking(PIO pio, uint sm, uint32_t data) {
    (void)pio; (void)sm;
    if (!g_drive) return;

    if (g_drive->write_capture_count >= g_drive->write_capture_capacity) {
        uint32_t cap = g_drive->write_capture_capacity ? g_drive->write_capture_capacity * 2 : 4096;
        g_drive->write_capture = realloc(g_drive->write_capture, cap);
        g_drive->write_capture_capacity = cap;
    }
    g_drive->write_capture[g_drive->write_capture_count++] = data & 0xFF;
}

static pio_hw_t pio0_hw = { .id = 0 };
static pio_hw_t pio1_hw = { .id = 1 };
PIO pio0 = &pio0_hw;
PIO pio1 = &pio1_hw;

const pio_program_t flux_read_program = {0};
const pio_program_t flux_write_program = {0};

void flux_read_program_init(PIO pio, uint sm, uint offset, uint pin, uint index_pin) {
    (void)pio; (void)sm; (void)offset; (void)pin; (void)index_pin;
}

void flux_write_program_init(PIO pio, uint sm, uint offset, uint pin) {
    (void)pio; (void)sm; (void)offset; (void)pin;
}
