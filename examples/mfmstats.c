#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "floppy.h"
#include "mfm_decode.h"

#define PULSE_BINS 128

static inline bool flux_data_available(floppy_t *f) {
    return f->read.half || !pio_sm_is_rx_fifo_empty(f->read.pio, f->read.sm);
}

static inline uint16_t flux_read_raw(floppy_t *f) {
    if (f->read.half) {
        uint16_t v = f->read.half;
        f->read.half = 0;
        return v;
    }
    uint32_t pv = pio_sm_get_blocking(f->read.pio, f->read.sm);
    f->read.half = pv >> 16;
    return pv & 0xffff;
}

static inline uint16_t flux_read_wait(floppy_t *f) {
    while (!flux_data_available(f)) {
        tight_loop_contents();
    }
    return flux_read_raw(f);
}

typedef struct {
    uint32_t short_count;
    uint32_t medium_count;
    uint32_t long_count;
    uint32_t invalid_count;
    uint32_t total_pulses;
    uint32_t histogram[PULSE_BINS];
    uint16_t T2_max;
    uint16_t T3_max;
    uint32_t syncs;
    uint32_t sectors;
    uint32_t crc_errors;
} track_stats_t;

static void gpio_put_oc(uint pin, bool value) {
    if (value == 0) {
        gpio_put(pin, 0);
        gpio_set_dir(pin, GPIO_OUT);
    } else {
        gpio_set_dir(pin, GPIO_IN);
    }
}

static void read_track_stats(floppy_t *f, int track, int side, track_stats_t *stats) {
    memset(stats, 0, sizeof(*stats));

    floppy_seek(f, track);
    gpio_put_oc(f->pins.side_select, side == 0 ? 1 : 0);

    pio_sm_exec(f->read.pio, f->read.sm, pio_encode_jmp(f->read.offset));
    pio_sm_restart(f->read.pio, f->read.sm);
    pio_sm_clear_fifos(f->read.pio, f->read.sm);
    f->read.half = 0;
    pio_sm_set_enabled(f->read.pio, f->read.sm, true);

    mfm_t mfm;
    mfm_init(&mfm);
    mfm_reset(&mfm);

    uint16_t prev = flux_read_wait(f) >> 1;
    bool ix_prev = false;
    sector_t sector;
    int ix_edges = 0;

    while (ix_edges < 6) {
        uint16_t value = flux_read_wait(f);
        uint8_t ix = value & 1;
        uint16_t cnt = value >> 1;
        int delta = prev - cnt;
        if (delta < 0) delta += 0x8000;

        if (ix != ix_prev) ix_edges++;
        ix_prev = ix;

        if (delta > 0 && delta < PULSE_BINS) {
            stats->histogram[delta]++;
        }
        stats->total_pulses++;

        if (delta < MFM_PULSE_FLOOR || delta >= MFM_PULSE_CEILING) {
            stats->invalid_count++;
        } else if (delta <= mfm.T2_max) {
            stats->short_count++;
        } else if (delta <= mfm.T3_max) {
            stats->medium_count++;
        } else {
            stats->long_count++;
        }

        mfm_feed(&mfm, delta, &sector);
        prev = cnt;
    }

    pio_sm_set_enabled(f->read.pio, f->read.sm, false);

    stats->T2_max = mfm.T2_max;
    stats->T3_max = mfm.T3_max;
    stats->syncs = mfm.syncs_found;
    stats->sectors = mfm.sectors_read;
    stats->crc_errors = mfm.crc_errors;
}

static void print_histogram(track_stats_t *stats) {
    uint32_t peak = 0;
    for (int i = 0; i < PULSE_BINS; i++) {
        if (stats->histogram[i] > peak) peak = stats->histogram[i];
    }
    if (peak == 0) return;

    int first = 0, last = PULSE_BINS - 1;
    while (first < PULSE_BINS && stats->histogram[first] == 0) first++;
    while (last > first && stats->histogram[last] == 0) last--;

    printf("  Pulse Distribution (delta ticks):\n");
    for (int i = first; i <= last; i++) {
        if (stats->histogram[i] == 0) continue;
        int bar = (stats->histogram[i] * 50) / peak;
        printf("  %3d: %6lu |", i, stats->histogram[i]);
        for (int j = 0; j < bar; j++) printf("#");
        printf("\n");
    }
}

static void print_track_stats(int track, int side, track_stats_t *stats) {
    printf("\n  Track %d Side %d:\n", track, side);
    printf("    Pulses:   %lu total\n", stats->total_pulses);
    printf("    Short:    %lu (%.1f%%)\n", stats->short_count,
           stats->total_pulses ? stats->short_count * 100.0 / stats->total_pulses : 0);
    printf("    Medium:   %lu (%.1f%%)\n", stats->medium_count,
           stats->total_pulses ? stats->medium_count * 100.0 / stats->total_pulses : 0);
    printf("    Long:     %lu (%.1f%%)\n", stats->long_count,
           stats->total_pulses ? stats->long_count * 100.0 / stats->total_pulses : 0);
    printf("    Invalid:  %lu (%.1f%%)\n", stats->invalid_count,
           stats->total_pulses ? stats->invalid_count * 100.0 / stats->total_pulses : 0);
    printf("    Syncs:    %lu\n", stats->syncs);
    printf("    Sectors:  %lu / %d\n", stats->sectors, SECTORS_PER_TRACK);
    printf("    CRC err:  %lu\n", stats->crc_errors);
    printf("    Adaptive: T2_max=%d  T3_max=%d\n", stats->T2_max, stats->T3_max);
}

int main(void) {
    stdio_init_all();
    sleep_ms(3000);

    printf("\n\n========================================\n");
    printf("  MFM SIGNAL QUALITY ANALYZER\n");
    printf("========================================\n\n");

    floppy_t floppy = {
        .pins = {
            .index         = 2,
            .track0        = 3,
            .write_protect = 4,
            .read_data     = 5,
            .disk_change   = 6,
            .drive_select  = 7,
            .motor_enable  = 8,
            .direction     = 9,
            .step          = 10,
            .write_data    = 11,
            .write_gate    = 12,
            .side_select   = 13,
            .density       = 14,
        }
    };

    floppy_init(&floppy);
    floppy_set_density(&floppy, true);

    printf("[INIT] HD mode\n\n");

    track_stats_t stats;

    struct { int track; int side; const char *label; } targets[] = {
        {0,  0, "Track 0 (outermost)"},
        {39, 0, "Track 39 (mid-outer)"},
        {79, 0, "Track 79 (innermost)"},
    };

    for (int t = 0; t < 3; t++) {
        printf("========================================\n");
        printf("  %s\n", targets[t].label);
        printf("========================================\n");

        read_track_stats(&floppy, targets[t].track, targets[t].side, &stats);
        print_track_stats(targets[t].track, targets[t].side, &stats);
        print_histogram(&stats);
    }

    printf("\n========================================\n");
    printf("  PER-TRACK SUMMARY (all tracks, side 0)\n");
    printf("========================================\n");
    printf("  %-6s %-8s %-8s %-8s %-8s %-5s %-5s\n",
           "TRACK", "SHORT", "MEDIUM", "LONG", "INVALID", "SECT", "CRC");
    printf("  %-6s %-8s %-8s %-8s %-8s %-5s %-5s\n",
           "-----", "------", "------", "------", "-------", "----", "---");

    int total_sectors = 0;
    int total_crc = 0;

    for (int track = 0; track < FLOPPY_TRACKS; track++) {
        read_track_stats(&floppy, track, 0, &stats);
        printf("  T%02d    %-8lu %-8lu %-8lu %-8lu %-5lu %-5lu\n",
               track, stats.short_count, stats.medium_count,
               stats.long_count, stats.invalid_count,
               stats.sectors, stats.crc_errors);
        total_sectors += stats.sectors;
        total_crc += stats.crc_errors;
    }

    printf("\n  Side 0 total: %d sectors decoded, %d CRC errors\n", total_sectors, total_crc);

    printf("\n========================================\n");
    printf("  DONE\n");
    printf("========================================\n");

    for (;;) {
        sleep_ms(10000);
        printf(".\n");
    }

    return 0;
}
