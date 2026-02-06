#include "test.h"
#include "flux_sim.h"
#include "../src/mfm_decode.h"
#include "../src/mfm_encode.h"
#include <string.h>

static int decode_track(flux_sim_t *sim, sector_t *sectors, int max_sectors) {
    mfm_t mfm;
    mfm_init(&mfm);
    sector_t out;
    int found = 0;
    uint16_t delta;

    while (flux_sim_next(sim, &delta) && found < max_sectors) {
        if (mfm_feed(&mfm, delta, &out)) {
            if (out.valid && out.sector_n >= 1 && out.sector_n <= SECTORS_PER_TRACK) {
                sectors[found++] = out;
            }
        }
    }
    return found;
}

TEST(test_synthetic_single_sector) {
    uint8_t pulse_buf[8192];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    sector_t src = {.track = 0, .side = 0, .sector_n = 1, .size_code = 2, .valid = true};
    for (int i = 0; i < SECTOR_SIZE; i++) src.data[i] = i & 0xFF;

    mfm_encode_gap(&enc, 80);
    mfm_encode_sector(&enc, &src);
    mfm_encode_gap(&enc, 54);

    flux_sim_t sim;
    flux_sim_from_track(&sim, pulse_buf, enc.pos);

    sector_t sectors[1];
    int found = decode_track(&sim, sectors, 1);
    ASSERT_EQ(found, 1);
    ASSERT(sectors[0].valid);
    ASSERT_EQ(sectors[0].track, 0);
    ASSERT_EQ(sectors[0].sector_n, 1);
    ASSERT_MEM_EQ(sectors[0].data, src.data, SECTOR_SIZE);

    flux_sim_close(&sim);
}

TEST(test_synthetic_full_track) {
    uint8_t pulse_buf[200000];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    track_t trk = {.track = 5, .side = 1};
    for (int s = 0; s < SECTORS_PER_TRACK; s++) {
        trk.sectors[s].track = 5;
        trk.sectors[s].side = 1;
        trk.sectors[s].sector_n = s + 1;
        trk.sectors[s].valid = true;
        memset(trk.sectors[s].data, s * 13 + 7, SECTOR_SIZE);
    }

    mfm_encode_track(&enc, &trk);

    flux_sim_t sim;
    flux_sim_from_track(&sim, pulse_buf, enc.pos);

    sector_t sectors[SECTORS_PER_TRACK];
    int found = decode_track(&sim, sectors, SECTORS_PER_TRACK);
    ASSERT_EQ(found, SECTORS_PER_TRACK);

    for (int i = 0; i < SECTORS_PER_TRACK; i++) {
        ASSERT(sectors[i].valid);
    }

    flux_sim_close(&sim);
}

TEST(test_synthetic_with_jitter) {
    uint8_t pulse_buf[200000];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    track_t trk = {.track = 0, .side = 0};
    for (int s = 0; s < SECTORS_PER_TRACK; s++) {
        trk.sectors[s].track = 0;
        trk.sectors[s].side = 0;
        trk.sectors[s].sector_n = s + 1;
        trk.sectors[s].valid = true;
        memset(trk.sectors[s].data, s, SECTOR_SIZE);
    }

    mfm_encode_track(&enc, &trk);

    flux_sim_t sim;
    flux_sim_from_track(&sim, pulse_buf, enc.pos);
    flux_sim_set_jitter(&sim, 4, 12345);

    sector_t sectors[SECTORS_PER_TRACK];
    int found = decode_track(&sim, sectors, SECTORS_PER_TRACK);

    printf("\n  Sectors decoded with ±4 jitter: %d/%d\n  ", found, SECTORS_PER_TRACK);
    ASSERT(found >= 16);

    flux_sim_close(&sim);
}

TEST(test_synthetic_with_drift) {
    uint8_t pulse_buf[200000];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    track_t trk = {.track = 0, .side = 0};
    for (int s = 0; s < SECTORS_PER_TRACK; s++) {
        trk.sectors[s].track = 0;
        trk.sectors[s].side = 0;
        trk.sectors[s].sector_n = s + 1;
        trk.sectors[s].valid = true;
        memset(trk.sectors[s].data, 0xAA, SECTOR_SIZE);
    }

    mfm_encode_track(&enc, &trk);

    int drift_values[] = {-50000, -30000, -10000, 10000, 30000, 50000};
    for (int d = 0; d < 6; d++) {
        flux_sim_t sim;
        flux_sim_from_track(&sim, pulse_buf, enc.pos);
        flux_sim_set_drift(&sim, drift_values[d]);

        sector_t sectors[SECTORS_PER_TRACK];
        int found = decode_track(&sim, sectors, SECTORS_PER_TRACK);

        printf("\n  Drift %+d ppm: %d/%d sectors  ", drift_values[d], found, SECTORS_PER_TRACK);

        if (abs(drift_values[d]) <= 30000) {
            ASSERT(found >= 14);
        }

        flux_sim_close(&sim);
    }
    printf("\n  ");
}

TEST(test_synthetic_with_precomp) {
    uint8_t pulse_buf[200000];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    track_t trk = {.track = 60, .side = 0};
    for (int s = 0; s < SECTORS_PER_TRACK; s++) {
        trk.sectors[s].track = 60;
        trk.sectors[s].side = 0;
        trk.sectors[s].sector_n = s + 1;
        trk.sectors[s].valid = true;
        for (int i = 0; i < SECTOR_SIZE; i++) {
            trk.sectors[s].data[i] = (s * 37 + i) & 0xFF;
        }
    }

    mfm_encode_track(&enc, &trk);

    flux_sim_t sim;
    flux_sim_from_track(&sim, pulse_buf, enc.pos);

    sector_t sectors[SECTORS_PER_TRACK];
    int found = decode_track(&sim, sectors, SECTORS_PER_TRACK);

    printf("\n  Precomp track 60: %d/%d sectors\n  ", found, SECTORS_PER_TRACK);
    ASSERT_EQ(found, SECTORS_PER_TRACK);

    for (int i = 0; i < found; i++) {
        ASSERT(sectors[i].valid);
        ASSERT_EQ(sectors[i].track, 60);
    }

    flux_sim_close(&sim);
}

TEST(test_adaptive_timing_with_drift) {
    uint8_t pulse_buf[8192];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    sector_t src = {.track = 0, .side = 0, .sector_n = 1, .size_code = 2, .valid = true};
    memset(src.data, 0x42, SECTOR_SIZE);

    mfm_encode_gap(&enc, 80);
    mfm_encode_sector(&enc, &src);
    mfm_encode_gap(&enc, 54);

    flux_sim_t sim;
    flux_sim_from_track(&sim, pulse_buf, enc.pos);
    flux_sim_set_drift(&sim, 80000);

    sector_t sectors[1];
    int found = decode_track(&sim, sectors, 1);

    printf("\n  +8%% drift with adaptive timing: %d sector(s)\n  ", found);
    ASSERT_EQ(found, 1);
    ASSERT(sectors[0].valid);

    flux_sim_close(&sim);
}

#define SCP_PATH "../../system-shock-multilingual-floppy-ibm-pc/disk1.scp"

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

TEST(test_scp_decode_track0) {
    size_t size;
    uint8_t *data = load_file(SCP_PATH, &size);
    if (!data) {
        printf("SKIP (no SCP file)\n  ");
        tests_passed++;
        return;
    }

    flux_sim_t sim;
    ASSERT(flux_sim_open_scp(&sim, data, size));
    ASSERT(flux_sim_seek(&sim, 0, 0, 0));

    sector_t sectors[SECTORS_PER_TRACK];
    int found = decode_track(&sim, sectors, SECTORS_PER_TRACK);

    printf("\n  Track 0 side 0: %d/%d sectors decoded\n  ", found, SECTORS_PER_TRACK);

    bool seen[SECTORS_PER_TRACK] = {0};
    for (int i = 0; i < found; i++) {
        ASSERT_EQ(sectors[i].track, 0);
        ASSERT_EQ(sectors[i].side, 0);
        ASSERT(sectors[i].sector_n >= 1 && sectors[i].sector_n <= SECTORS_PER_TRACK);
        seen[sectors[i].sector_n - 1] = true;
    }

    int unique = 0;
    for (int i = 0; i < SECTORS_PER_TRACK; i++) {
        if (seen[i]) unique++;
    }
    printf("  Unique sectors: %d/%d\n  ", unique, SECTORS_PER_TRACK);
    ASSERT(unique >= 14);

    flux_sim_close(&sim);
    free(data);
}

TEST(test_scp_decode_full_disk) {
    size_t size;
    uint8_t *data = load_file(SCP_PATH, &size);
    if (!data) {
        printf("SKIP (no SCP file)\n  ");
        tests_passed++;
        return;
    }

    flux_sim_t sim;
    ASSERT(flux_sim_open_scp(&sim, data, size));

    int total_sectors = 0;
    int total_tracks = 0;
    int perfect_tracks = 0;

    for (int track = 0; track < 80; track++) {
        for (int side = 0; side < 2; side++) {
            bool seen[SECTORS_PER_TRACK] = {0};
            int best = 0;

            for (int rev = 0; rev < sim.num_revolutions; rev++) {
                if (!flux_sim_seek(&sim, track, side, rev)) continue;

                sector_t sectors[SECTORS_PER_TRACK];
                int found = decode_track(&sim, sectors, SECTORS_PER_TRACK);

                for (int i = 0; i < found; i++) {
                    if (sectors[i].sector_n >= 1 && sectors[i].sector_n <= SECTORS_PER_TRACK) {
                        seen[sectors[i].sector_n - 1] = true;
                    }
                }
            }

            for (int i = 0; i < SECTORS_PER_TRACK; i++) {
                if (seen[i]) best++;
            }

            total_sectors += best;
            total_tracks++;
            if (best == SECTORS_PER_TRACK) perfect_tracks++;
        }
    }

    printf("\n  Full disk decode: %d/%d sectors (%d/%d perfect tracks)\n  ",
           total_sectors, total_tracks * SECTORS_PER_TRACK,
           perfect_tracks, total_tracks);

    ASSERT(total_sectors >= total_tracks * SECTORS_PER_TRACK * 95 / 100);

    flux_sim_close(&sim);
    free(data);
}

TEST(test_scp_boot_sector_content) {
    size_t size;
    uint8_t *data = load_file(SCP_PATH, &size);
    if (!data) {
        printf("SKIP (no SCP file)\n  ");
        tests_passed++;
        return;
    }

    flux_sim_t sim;
    ASSERT(flux_sim_open_scp(&sim, data, size));

    sector_t boot = {0};
    for (int rev = 0; rev < sim.num_revolutions && !boot.valid; rev++) {
        if (!flux_sim_seek(&sim, 0, 0, rev)) continue;
        mfm_t mfm;
        mfm_init(&mfm);
        uint16_t delta;
        while (flux_sim_next(&sim, &delta)) {
            if (mfm_feed(&mfm, delta, &boot)) {
                if (boot.valid && boot.sector_n == 1) break;
                boot.valid = false;
            }
        }
    }

    ASSERT(boot.valid);
    ASSERT_EQ(boot.data[510], 0x55);
    ASSERT_EQ(boot.data[511], 0xAA);
    ASSERT_EQ(boot.data[11] | (boot.data[12] << 8), 512);
    ASSERT_EQ(boot.data[21], 0xF0);

    printf("\n  Boot sector: sig=%02X%02X, bps=%d, media=%02X, spc=%d, fats=%d\n  ",
           boot.data[510], boot.data[511],
           boot.data[11] | (boot.data[12] << 8),
           boot.data[21], boot.data[13], boot.data[16]);

    flux_sim_close(&sim);
    free(data);
}

int main(void) {
    printf("=== Flux Simulator Tests ===\n\n");

    printf("--- Synthetic Tests ---\n");
    RUN_TEST(test_synthetic_single_sector);
    RUN_TEST(test_synthetic_full_track);
    RUN_TEST(test_synthetic_with_jitter);
    RUN_TEST(test_synthetic_with_drift);
    RUN_TEST(test_synthetic_with_precomp);
    RUN_TEST(test_adaptive_timing_with_drift);

    printf("\n--- Real SCP Tests (System Shock disk 1) ---\n");
    RUN_TEST(test_scp_decode_track0);
    RUN_TEST(test_scp_decode_full_disk);
    RUN_TEST(test_scp_boot_sector_content);

    TEST_RESULTS();
}
