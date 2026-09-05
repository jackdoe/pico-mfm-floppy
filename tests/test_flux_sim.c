#include "test.h"
#include "flux_sim.h"
#include "scp_fixture.h"
#include "../src/mfm.h"
#include <string.h>

static int decode_track(flux_sim_t *sim, mfm_sector_t *sectors, int max_sectors) {
    mfm_t mfm;
    mfm_init(&mfm);
    mfm_sector_t out;
    int found = 0;
    uint16_t delta;

    while (flux_sim_next(sim, &delta) && found < max_sectors) {
        if (mfm_feed(&mfm, delta, &out)) {
            if (out.sector < DISK_SECTORS_PER_TRACK) {
                sectors[found++] = out;
            }
        }
    }
    return found;
}

static void assert_decoded_track(const mfm_sector_t *sectors, int found,
                                 const track_t *expected) {
    bool seen[DISK_SECTORS_PER_TRACK] = {0};
    ASSERT_EQ(found, DISK_SECTORS_PER_TRACK);
    for (int index = 0; index < found; index++) {
        const mfm_sector_t *sector = &sectors[index];
        ASSERT_EQ(sector->cylinder, expected->cylinder);
        ASSERT_EQ(sector->head, expected->head);
        ASSERT(sector->sector < DISK_SECTORS_PER_TRACK);
        ASSERT(!seen[sector->sector]);
        seen[sector->sector] = true;
        ASSERT_MEM_EQ(sector->data, expected->data[sector->sector],
                      DISK_SECTOR_SIZE);
    }
}

TEST(test_synthetic_single_sector) {
    uint8_t pulse_buf[8192];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    uint8_t src[DISK_SECTOR_SIZE];
    for (size_t i = 0; i < DISK_SECTOR_SIZE; i++) src[i] = i & 0xFFu;

    mfm_encode_gap(&enc, 80);
    mfm_encode_sector(&enc, 0, 0, 0, src);
    mfm_encode_gap(&enc, 54);

    flux_sim_t sim;
    flux_sim_from_track(&sim, pulse_buf, enc.pos);

    mfm_sector_t sectors[1];
    int found = decode_track(&sim, sectors, 1);
    ASSERT_EQ(found, 1);
    ASSERT_EQ(sectors[0].cylinder, 0);
    ASSERT_EQ(sectors[0].head, 0);
    ASSERT_EQ(sectors[0].sector, 0);
    ASSERT_MEM_EQ(sectors[0].data, src, DISK_SECTOR_SIZE);

    flux_sim_close(&sim);
}

TEST(test_synthetic_full_track) {
    uint8_t pulse_buf[200000];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    track_t trk = {.cylinder = 5, .head = 1, .valid = DISK_TRACK_VALID};
    for (uint8_t s = 0; s < DISK_SECTORS_PER_TRACK; s++) {
        memset(trk.data[s], s * 13 + 7, DISK_SECTOR_SIZE);
    }

    mfm_encode_track(&enc, &trk);

    flux_sim_t sim;
    flux_sim_from_track(&sim, pulse_buf, enc.pos);

    mfm_sector_t sectors[DISK_SECTORS_PER_TRACK];
    int found = decode_track(&sim, sectors, DISK_SECTORS_PER_TRACK);
    assert_decoded_track(sectors, found, &trk);

    flux_sim_close(&sim);
}

TEST(test_synthetic_with_jitter) {
    uint8_t pulse_buf[200000];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    track_t trk = {.cylinder = 0, .head = 0, .valid = DISK_TRACK_VALID};
    for (uint8_t s = 0; s < DISK_SECTORS_PER_TRACK; s++) {
        memset(trk.data[s], s, DISK_SECTOR_SIZE);
    }

    mfm_encode_track(&enc, &trk);

    flux_sim_t sim;
    flux_sim_from_track(&sim, pulse_buf, enc.pos);
    ASSERT(flux_sim_set_noise(&sim, (flux_noise_config_t){
        .seed = 12345,
        .jitter_ticks = 4,
    }));

    mfm_sector_t sectors[DISK_SECTORS_PER_TRACK];
    int found = decode_track(&sim, sectors, DISK_SECTORS_PER_TRACK);

    printf("\n  Sectors decoded with ±4 jitter: %d/%u\n  ", found,
           DISK_SECTORS_PER_TRACK);
    assert_decoded_track(sectors, found, &trk);

    flux_sim_close(&sim);
}

TEST(test_noise_is_seeded_reproducible_and_validated) {
    uint8_t pulse_buf[4096];
    for (size_t i = 0; i < sizeof(pulse_buf); i++) {
        pulse_buf[i] = (uint8_t)(24u + i % 3u * 24u);
    }
    flux_sim_t first;
    flux_sim_t second;
    flux_sim_t different;
    ASSERT(flux_sim_from_track(&first, pulse_buf, sizeof(pulse_buf)));
    ASSERT(flux_sim_from_track(&second, pulse_buf, sizeof(pulse_buf)));
    ASSERT(flux_sim_from_track(&different, pulse_buf, sizeof(pulse_buf)));
    flux_noise_config_t config = {
        .seed = UINT32_C(0x13579BDF),
        .jitter_ticks = 3,
        .drift_ppm = 15000,
        .wander_step_ppm = 500,
        .wander_limit_ppm = 5000,
        .wander_period = 64,
        .impulse_rate_ppm = 5000,
        .impulse_ticks = 8,
    };
    ASSERT(flux_sim_set_noise(&first, config));
    ASSERT(flux_sim_set_noise(&second, config));
    config.seed++;
    ASSERT(flux_sim_set_noise(&different, config));

    uint32_t changed = 0;
    uint32_t diverged = 0;
    for (size_t i = 0; i < sizeof(pulse_buf); i++) {
        uint16_t a;
        uint16_t b;
        uint16_t c;
        ASSERT(flux_sim_next(&first, &a));
        ASSERT(flux_sim_next(&second, &b));
        ASSERT(flux_sim_next(&different, &c));
        ASSERT_EQ(a, b);
        uint16_t clean = (uint16_t)(pulse_buf[i]);
        if (a != clean) changed++;
        if (a != c) diverged++;
    }
    ASSERT(changed != 0);
    ASSERT(diverged != 0);
    ASSERT(!flux_sim_next(&first, &(uint16_t){0}));

    config.drift_ppm = -(int32_t)FLUX_NOISE_RATE_SCALE;
    ASSERT(!flux_sim_set_noise(&first, config));
    flux_sim_close(&first);
    flux_sim_close(&second);
    flux_sim_close(&different);
}

TEST(test_synthetic_with_correlated_noise) {
    uint8_t pulse_buf[200000];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));
    track_t trk = {.cylinder = 25, .head = 1, .valid = DISK_TRACK_VALID};
    for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
        for (size_t byte = 0; byte < DISK_SECTOR_SIZE; byte++) {
            trk.data[sector][byte] =
                (uint8_t)((size_t)sector * 29u + byte * 7u);
        }
    }
    mfm_encode_track(&enc, &trk);
    flux_sim_t sim;
    ASSERT(flux_sim_from_track(&sim, pulse_buf, enc.pos));
    ASSERT(flux_sim_set_noise(&sim, (flux_noise_config_t){
        .seed = UINT32_C(0x2468ACE1),
        .jitter_ticks = 3,
        .drift_ppm = 15000,
        .wander_step_ppm = 250,
        .wander_limit_ppm = 8000,
        .wander_period = 256,
    }));
    mfm_sector_t sectors[DISK_SECTORS_PER_TRACK];
    int found = decode_track(&sim, sectors, DISK_SECTORS_PER_TRACK);
    assert_decoded_track(sectors, found, &trk);
    flux_sim_close(&sim);
}

TEST(test_excessive_noise_fails_closed) {
    uint8_t pulse_buf[200000];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));
    track_t trk = {.cylinder = 26, .head = 0, .valid = DISK_TRACK_VALID};
    for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
        memset(trk.data[sector], sector, DISK_SECTOR_SIZE);
    }
    mfm_encode_track(&enc, &trk);
    flux_sim_t sim;
    ASSERT(flux_sim_from_track(&sim, pulse_buf, enc.pos));
    ASSERT(flux_sim_set_noise(&sim, (flux_noise_config_t){
        .seed = UINT32_C(0xDEADBEEF),
        .jitter_ticks = 12,
        .drift_ppm = 200000,
        .wander_step_ppm = 5000,
        .wander_limit_ppm = 100000,
        .wander_period = 16,
        .impulse_rate_ppm = FLUX_NOISE_RATE_SCALE,
        .impulse_ticks = 40,
    }));
    mfm_sector_t sectors[DISK_SECTORS_PER_TRACK];
    int found = decode_track(&sim, sectors, DISK_SECTORS_PER_TRACK);
    ASSERT(found < (int)DISK_SECTORS_PER_TRACK);
    flux_sim_close(&sim);
}

TEST(test_synthetic_with_drift) {
    uint8_t pulse_buf[200000];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    track_t trk = {.cylinder = 0, .head = 0, .valid = DISK_TRACK_VALID};
    for (uint8_t s = 0; s < DISK_SECTORS_PER_TRACK; s++) {
        memset(trk.data[s], 0xAA, DISK_SECTOR_SIZE);
    }

    mfm_encode_track(&enc, &trk);

    int drift_values[] = {
        -80000, -50000, -30000, -10000, 10000, 30000, 50000, 80000,
    };
    for (size_t d = 0; d < sizeof(drift_values) / sizeof(drift_values[0]); d++) {
        flux_sim_t sim;
        flux_sim_from_track(&sim, pulse_buf, enc.pos);
        ASSERT(flux_sim_set_noise(&sim, (flux_noise_config_t){
            .drift_ppm = drift_values[d],
        }));

        mfm_sector_t sectors[DISK_SECTORS_PER_TRACK];
        int found = decode_track(&sim, sectors, DISK_SECTORS_PER_TRACK);

        printf("\n  Drift %+d ppm: %d/%u sectors  ", drift_values[d], found,
               DISK_SECTORS_PER_TRACK);

        assert_decoded_track(sectors, found, &trk);

        flux_sim_close(&sim);
    }
    printf("\n  ");
}

TEST(test_synthetic_with_precomp) {
    uint8_t pulse_buf[200000];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    track_t trk = {.cylinder = 60, .head = 0, .valid = DISK_TRACK_VALID};
    for (uint8_t s = 0; s < DISK_SECTORS_PER_TRACK; s++) {
        for (size_t i = 0; i < DISK_SECTOR_SIZE; i++) {
            trk.data[s][i] = (s * 37 + i) & 0xFF;
        }
    }

    mfm_encode_track(&enc, &trk);

    flux_sim_t sim;
    flux_sim_from_track(&sim, pulse_buf, enc.pos);

    mfm_sector_t sectors[DISK_SECTORS_PER_TRACK];
    int found = decode_track(&sim, sectors, DISK_SECTORS_PER_TRACK);

    printf("\n  Precomp track 60: %d/%u sectors\n  ", found,
           DISK_SECTORS_PER_TRACK);
    assert_decoded_track(sectors, found, &trk);

    flux_sim_close(&sim);
}

TEST(test_adaptive_timing_with_drift) {
    uint8_t pulse_buf[8192];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    uint8_t src[DISK_SECTOR_SIZE];
    memset(src, 0x42, sizeof(src));

    mfm_encode_gap(&enc, 80);
    mfm_encode_sector(&enc, 0, 0, 0, src);
    mfm_encode_gap(&enc, 54);

    flux_sim_t sim;
    flux_sim_from_track(&sim, pulse_buf, enc.pos);
    ASSERT(flux_sim_set_noise(&sim, (flux_noise_config_t){
        .drift_ppm = 80000,
    }));

    mfm_sector_t sectors[1];
    int found = decode_track(&sim, sectors, 1);

    printf("\n  +8%% drift with adaptive timing: %d sector(s)\n  ", found);
    ASSERT_EQ(found, 1);

    flux_sim_close(&sim);
}

static const char *scp_path;

TEST(test_scp_decode_track0) {
    size_t size;
    uint8_t *data = scp_fixture_load(scp_path, &size);
    ASSERT_NOT_NULL(data);

    flux_sim_t sim;
    ASSERT(flux_sim_open_scp(&sim, data, size));
    ASSERT(flux_sim_seek(&sim, 0, 0, 0));

    mfm_sector_t sectors[DISK_SECTORS_PER_TRACK];
    int found = decode_track(&sim, sectors, DISK_SECTORS_PER_TRACK);

    printf("\n  Track 0 side 0: %d/%u sectors decoded\n  ", found,
           DISK_SECTORS_PER_TRACK);

    bool seen[DISK_SECTORS_PER_TRACK] = {0};
    for (int i = 0; i < found; i++) {
        ASSERT_EQ(sectors[i].cylinder, 0);
        ASSERT_EQ(sectors[i].head, 0);
        ASSERT(sectors[i].sector < DISK_SECTORS_PER_TRACK);
        seen[sectors[i].sector] = true;
    }

    int unique = 0;
    for (uint8_t i = 0; i < DISK_SECTORS_PER_TRACK; i++) {
        if (seen[i]) unique++;
    }
    printf("  Unique sectors: %d/%u\n  ", unique, DISK_SECTORS_PER_TRACK);
    ASSERT_EQ(unique, DISK_SECTORS_PER_TRACK);

    flux_sim_close(&sim);
    free(data);
}

TEST(test_scp_track0_with_correlated_noise) {
    size_t size;
    uint8_t *data = scp_fixture_load(scp_path, &size);
    ASSERT_NOT_NULL(data);
    flux_sim_t sim;
    ASSERT(flux_sim_open_scp(&sim, data, size));
    ASSERT(flux_sim_seek(&sim, 0, 0, 0));
    ASSERT(flux_sim_set_noise(&sim, (flux_noise_config_t){
        .seed = UINT32_C(0x31415926),
        .jitter_ticks = 2,
        .drift_ppm = 10000,
        .wander_step_ppm = 200,
        .wander_limit_ppm = 5000,
        .wander_period = 256,
    }));
    mfm_sector_t sectors[DISK_SECTORS_PER_TRACK];
    int found = decode_track(&sim, sectors, DISK_SECTORS_PER_TRACK);
    ASSERT_EQ(found, DISK_SECTORS_PER_TRACK);
    bool seen[DISK_SECTORS_PER_TRACK] = {0};
    for (int i = 0; i < found; i++) {
        ASSERT_EQ(sectors[i].cylinder, 0);
        ASSERT_EQ(sectors[i].head, 0);
        ASSERT(sectors[i].sector < DISK_SECTORS_PER_TRACK);
        ASSERT(!seen[sectors[i].sector]);
        seen[sectors[i].sector] = true;
    }
    flux_sim_close(&sim);
    free(data);
}

TEST(test_scp_decode_full_disk) {
    size_t size;
    uint8_t *data = scp_fixture_load(scp_path, &size);
    ASSERT_NOT_NULL(data);

    flux_sim_t sim;
    ASSERT(flux_sim_open_scp(&sim, data, size));

    unsigned total_sectors = 0;
    unsigned total_tracks = 0;
    unsigned perfect_tracks = 0;

    for (uint8_t track = 0; track < DISK_CYLINDERS; track++) {
        for (uint8_t side = 0; side < DISK_HEADS; side++) {
            bool seen[DISK_SECTORS_PER_TRACK] = {0};
            unsigned best = 0;

            for (uint8_t rev = 0; rev < sim.num_revolutions; rev++) {
                if (!flux_sim_seek(&sim, track, side, rev)) continue;

                mfm_sector_t sectors[DISK_SECTORS_PER_TRACK];
                int found = decode_track(&sim, sectors, DISK_SECTORS_PER_TRACK);

                for (int i = 0; i < found; i++) {
                    if (sectors[i].sector < DISK_SECTORS_PER_TRACK) {
                        seen[sectors[i].sector] = true;
                    }
                }
            }

            for (uint8_t i = 0; i < DISK_SECTORS_PER_TRACK; i++) {
                if (seen[i]) best++;
            }

            total_sectors += best;
            total_tracks++;
            if (best == DISK_SECTORS_PER_TRACK) perfect_tracks++;
        }
    }

    printf("\n  Full disk decode: %u/%u sectors (%u/%u perfect tracks)\n  ",
           total_sectors, total_tracks * DISK_SECTORS_PER_TRACK,
           perfect_tracks, total_tracks);

    ASSERT_EQ(total_sectors, DISK_SECTOR_COUNT);
    ASSERT_EQ(perfect_tracks, DISK_TRACK_COUNT);

    flux_sim_close(&sim);
    free(data);
}

TEST(test_scp_boot_sector_content) {
    size_t size;
    uint8_t *data = scp_fixture_load(scp_path, &size);
    ASSERT_NOT_NULL(data);

    flux_sim_t sim;
    ASSERT(flux_sim_open_scp(&sim, data, size));

    mfm_sector_t boot = {0};
    bool found = false;
    for (uint8_t rev = 0; rev < sim.num_revolutions && !found; rev++) {
        if (!flux_sim_seek(&sim, 0, 0, rev)) continue;
        mfm_t mfm;
        mfm_init(&mfm);
        uint16_t delta;
        while (flux_sim_next(&sim, &delta)) {
            if (mfm_feed(&mfm, delta, &boot)) {
                if (boot.cylinder == 0 && boot.head == 0 && boot.sector == 0) {
                    found = true;
                    break;
                }
            }
        }
    }

    ASSERT(found);
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

int main(int argc, char **argv) {
    if (argc != 1 && (argc != 3 || strcmp(argv[1], "--fixture") != 0)) {
        fprintf(stderr, "usage: %s [--fixture PATH]\n", argv[0]);
        return 2;
    }
    if (argc == 3) scp_path = argv[2];
    printf("=== Flux Simulator Tests ===\n\n");

    printf("--- Synthetic Tests ---\n");
    RUN_TEST(test_synthetic_single_sector);
    RUN_TEST(test_synthetic_full_track);
    RUN_TEST(test_synthetic_with_jitter);
    RUN_TEST(test_noise_is_seeded_reproducible_and_validated);
    RUN_TEST(test_synthetic_with_correlated_noise);
    RUN_TEST(test_excessive_noise_fails_closed);
    RUN_TEST(test_synthetic_with_drift);
    RUN_TEST(test_synthetic_with_precomp);
    RUN_TEST(test_adaptive_timing_with_drift);

    if (scp_path) {
        printf("\n--- Real SCP Tests (System Shock disk 1) ---\n");
        RUN_TEST(test_scp_decode_track0);
        RUN_TEST(test_scp_track0_with_correlated_noise);
        RUN_TEST(test_scp_decode_full_disk);
        RUN_TEST(test_scp_boot_sector_content);
    }

    TEST_RESULTS();
}
