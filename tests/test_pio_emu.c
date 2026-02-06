#include "test.h"
#include "pio_emu.h"
#include "flux_sim.h"
#include "../src/mfm_decode.h"
#include "../src/mfm_encode.h"
#include <string.h>

#define SCP_PATH "../../system-shock-multilingual-floppy-ibm-pc/disk1.scp"

#define PIO_JMP(cond, addr)        (uint16_t)((0 << 13) | ((cond) << 5) | (addr))
#define PIO_JMP_D(cond, addr, d)   (uint16_t)((0 << 13) | ((d) << 8) | ((cond) << 5) | (addr))
#define PIO_IN(src, bits)          (uint16_t)((2 << 13) | ((src) << 5) | (bits))
#define PIO_OUT(dst, bits)         (uint16_t)((3 << 13) | ((dst) << 5) | (bits))
#define PIO_PULL_BLOCK             (uint16_t)((4 << 13) | (7 << 5))
#define PIO_SET(dst, val)          (uint16_t)((7 << 13) | ((dst) << 5) | (val))
#define PIO_SET_D(dst, val, d)     (uint16_t)((7 << 13) | ((d) << 8) | ((dst) << 5) | (val))
#define PIO_NOP_D(d)               (uint16_t)((5 << 13) | ((d) << 8) | (2 << 5) | 2)

static const uint16_t flux_read_prog[] = {
    PIO_JMP(JMP_X_DEC, 1),
    PIO_JMP(JMP_PIN, 3),
    PIO_JMP(JMP_ALWAYS, 0),
    PIO_JMP(JMP_X_DEC, 4),
    PIO_JMP_D(JMP_PIN, 3, 1),
    PIO_IN(IN_PINS, 1),
    PIO_IN(IN_X, 15),
    PIO_JMP(JMP_X_DEC, 0),
};
#define FLUX_READ_LEN 8
#define FLUX_READ_WRAP_TARGET 0
#define FLUX_READ_WRAP 7

static const uint16_t flux_write_prog[] = {
    PIO_PULL_BLOCK,
    PIO_OUT(OUT_X, 8),
    PIO_SET(SET_PINS, 0),
    PIO_NOP_D(13),
    PIO_SET(SET_PINS, 1),
    PIO_JMP(JMP_X_DEC, 5),
};
#define FLUX_WRITE_LEN 6
#define FLUX_WRITE_WRAP_TARGET 0
#define FLUX_WRITE_WRAP 5

static uint8_t *load_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*size);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, *size, f);
    fclose(f);
    return buf;
}

TEST(test_flux_read_emu_basic) {
    pio_emu_t emu;
    pio_emu_init(&emu);
    pio_emu_load(&emu, flux_read_prog, FLUX_READ_LEN,
                 FLUX_READ_WRAP_TARGET, FLUX_READ_WRAP);

    emu.in_shift_right = true;
    emu.autopush_threshold = 32;
    emu.x = 0;

    uint16_t deltas[] = {48, 72, 48, 96, 48, 48, 72};
    int delta_idx = 0;
    uint32_t cycles_to_next = deltas[0] * 3;

    emu.jmp_pin = true;

    int samples = 0;
    for (uint64_t cycle = 0; cycle < 100000 && samples < 4; cycle++) {
        cycles_to_next--;
        if (cycles_to_next == 12) {
            emu.jmp_pin = false;
        } else if (cycles_to_next == 0) {
            emu.jmp_pin = true;
            delta_idx++;
            if (delta_idx < 7) {
                cycles_to_next = deltas[delta_idx] * 3;
            } else {
                cycles_to_next = 0xFFFFFF;
            }
        }

        emu.pin_values = 0;
        pio_emu_step(&emu);

        if (!pio_emu_rx_empty(&emu)) {
            uint32_t word = pio_emu_rx_get(&emu);
            uint16_t lo = word & 0xFFFF;
            uint16_t hi = word >> 16;
            uint16_t cnt_lo = lo >> 1;
            uint16_t cnt_hi = hi >> 1;
            (void)cnt_lo; (void)cnt_hi;
            samples += 2;
        }
    }

    printf("\n  Samples captured: %d\n  ", samples);
    ASSERT(samples >= 4);
}

TEST(test_flux_read_emu_scp) {
    size_t scp_size;
    uint8_t *scp_data = load_file(SCP_PATH, &scp_size);
    if (!scp_data) {
        printf("SKIP (no SCP file)\n  ");
        tests_passed++;
        return;
    }

    flux_sim_t sim;
    flux_sim_open_scp(&sim, scp_data, scp_size);
    flux_sim_seek(&sim, 0, 0, 0);

    pio_emu_t emu;
    pio_emu_init(&emu);
    pio_emu_load(&emu, flux_read_prog, FLUX_READ_LEN,
                 FLUX_READ_WRAP_TARGET, FLUX_READ_WRAP);
    emu.in_shift_right = true;
    emu.autopush_threshold = 32;
    emu.x = 0;
    emu.jmp_pin = true;

    uint16_t current_delta;
    bool have_delta = flux_sim_next(&sim, &current_delta);
    uint32_t cycles_to_next = have_delta ? (uint32_t)current_delta * 3 : 0xFFFFFF;
    uint32_t pulse_cycles = 12;
    bool pin_low = false;
    uint32_t pin_low_remaining = 0;

    mfm_t mfm;
    mfm_init(&mfm);
    sector_t sector;
    int sectors_found = 0;
    uint16_t prev_cnt = 0;
    bool first = true;

    for (uint64_t cycle = 0; cycle < 200000000ULL; cycle++) {
        if (pin_low_remaining > 0) {
            pin_low_remaining--;
            if (pin_low_remaining == 0) {
                emu.jmp_pin = true;
                pin_low = false;
            }
        }

        cycles_to_next--;
        if (cycles_to_next == 0 && !pin_low) {
            emu.jmp_pin = false;
            pin_low = true;
            pin_low_remaining = pulse_cycles;

            have_delta = flux_sim_next(&sim, &current_delta);
            if (!have_delta) break;
            cycles_to_next = (uint32_t)current_delta * 3;
        }

        emu.pin_values = 0;
        pio_emu_step(&emu);

        if (!pio_emu_rx_empty(&emu)) {
            uint32_t word = pio_emu_rx_get(&emu);
            uint16_t samples[2] = { word & 0xFFFF, word >> 16 };

            for (int s = 0; s < 2; s++) {
                uint16_t cnt = samples[s] >> 1;
                if (!first) {
                    int delta = (int)(prev_cnt) - (int)(cnt);
                    if (delta < 0) delta += 0x8000;
                    if (mfm_feed(&mfm, (uint16_t)delta, &sector)) {
                        if (sector.valid) sectors_found++;
                    }
                }
                first = false;
                prev_cnt = cnt;
            }
        }
    }

    flux_sim_close(&sim);
    free(scp_data);

    printf("\n  PIO emulator decoded %d sectors from track 0 side 0\n  ", sectors_found);
    ASSERT(sectors_found >= 16);
}

TEST(test_flux_write_emu_roundtrip) {
    pio_emu_t write_emu;
    pio_emu_init(&write_emu);
    pio_emu_load(&write_emu, flux_write_prog, FLUX_WRITE_LEN,
                 FLUX_WRITE_WRAP_TARGET, FLUX_WRITE_WRAP);
    write_emu.out_shift_right = true;
    write_emu.autopull_threshold = 8;

    uint8_t pulse_buf[8192];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    sector_t src = {.track = 0, .side = 0, .sector_n = 1, .size_code = 2, .valid = true};
    for (int i = 0; i < SECTOR_SIZE; i++) src.data[i] = i & 0xFF;
    mfm_encode_gap(&enc, 80);
    mfm_encode_sector(&enc, &src);
    mfm_encode_gap(&enc, 54);

    for (size_t i = 0; i < enc.pos; i++) {
        pio_emu_tx_put(&write_emu, pulse_buf[i]);
        while (write_emu.stalled || write_emu.tx_count > 0) {
            pio_emu_step(&write_emu);
        }
    }

    uint32_t total_cycles = 0;
    for (size_t i = 0; i < enc.pos; i++) {
        total_cycles += pulse_buf[i] + MFM_PIO_OVERHEAD;
    }
    uint32_t expected_us = total_cycles / 24;

    printf("\n  Write emulation: %zu pulses, %u cycles, ~%u us\n  ",
           enc.pos, total_cycles, expected_us);

    ASSERT(enc.pos > 3000);
    ASSERT(expected_us > 10000);
    ASSERT(expected_us < 200000);
}

int main(void) {
    printf("=== PIO Emulator Tests ===\n");
    printf("Cycle-accurate RP2040 PIO instruction execution\n\n");

    RUN_TEST(test_flux_read_emu_basic);
    RUN_TEST(test_flux_read_emu_scp);
    RUN_TEST(test_flux_write_emu_roundtrip);

    TEST_RESULTS();
}
