#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

// Include decoder and encoder
#include "../src/floppy.h"
#include "../src/crc.h"
#include "../src/mfm_decode.h"
#include "../src/mfm_encode.h"

// ============== Test Helpers ==============

// Convert encoder pulse timing to decoder delta
// Encoder outputs PIO wait counts (with overhead subtracted)
// Decoder expects total time between transitions
// So we add back the overhead to simulate what the decoder would see
uint16_t pulse_to_delta(uint8_t pulse) {
    return pulse + MFM_PIO_OVERHEAD;
}

// Add jitter to pulse timing for realistic tests
uint16_t pulse_to_delta_jitter(uint8_t pulse, uint32_t *seed) {
    *seed = *seed * 1103515245 + 12345;
    int jitter = ((*seed >> 16) % 11) - 5;  // -5 to +5
    return pulse + MFM_PIO_OVERHEAD + jitter;
}

// ============== Tests ==============

int tests_run = 0;
int tests_passed = 0;

#define TEST(name) void name(void); \
  void name##_runner(void) { \
    printf("Running %s... ", #name); \
    tests_run++; \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
  } \
  void name(void)

#define RUN_TEST(name) name##_runner()

#define ASSERT(cond) do { \
  if (!(cond)) { \
    printf("FAIL\n  Assertion failed: %s\n  at %s:%d\n", #cond, __FILE__, __LINE__); \
    exit(1); \
  } \
} while(0)

#define ASSERT_EQ(a, b) do { \
  if ((a) != (b)) { \
    printf("FAIL\n  Expected %d == %d\n  at %s:%d\n", (int)(a), (int)(b), __FILE__, __LINE__); \
    exit(1); \
  } \
} while(0)

// Test: Encoder produces reasonable output size
TEST(test_encoder_basic) {
    uint8_t buf[1024];
    mfm_encode_t enc;
    mfm_encode_init(&enc, buf, sizeof(buf));

    // Encode a single byte after setting up state
    enc.prev_bit = 1;  // As if after sync
    uint8_t data = 0xFB;
    mfm_encode_bytes(&enc, &data, 1);

    printf("\n  Encoded 0xFB: %zu pulses\n  ", enc.pos);

    // 0xFB = 11111011 -> should produce ~7 transitions
    ASSERT(enc.pos > 0);
    ASSERT(enc.pos <= 8);  // At most 8 transitions for 8 bits
}

// Test: Sync pattern produces correct pulse sequence
TEST(test_encoder_sync) {
    uint8_t buf[1024];
    mfm_encode_t enc;
    mfm_encode_init(&enc, buf, sizeof(buf));

    mfm_encode_sync(&enc);

    printf("\n  Sync produced %zu pulses\n  ", enc.pos);

    // 12 bytes of 0x00 = 96 clock pulses (all short)
    // Plus 15 sync mark pulses
    // Total should be around 111
    ASSERT(enc.pos > 100);

    // Last 15 pulses should be M L M L M S L M L M S L M L M
    size_t sync_start = enc.pos - 15;
    ASSERT_EQ(buf[sync_start + 0], MFM_PULSE_MEDIUM);
    ASSERT_EQ(buf[sync_start + 1], MFM_PULSE_LONG);
    ASSERT_EQ(buf[sync_start + 2], MFM_PULSE_MEDIUM);
    ASSERT_EQ(buf[sync_start + 3], MFM_PULSE_LONG);
    ASSERT_EQ(buf[sync_start + 4], MFM_PULSE_MEDIUM);
    ASSERT_EQ(buf[sync_start + 5], MFM_PULSE_SHORT);
    ASSERT_EQ(buf[sync_start + 6], MFM_PULSE_LONG);
    ASSERT_EQ(buf[sync_start + 7], MFM_PULSE_MEDIUM);
    ASSERT_EQ(buf[sync_start + 8], MFM_PULSE_LONG);
    ASSERT_EQ(buf[sync_start + 9], MFM_PULSE_MEDIUM);
    ASSERT_EQ(buf[sync_start + 10], MFM_PULSE_SHORT);
    ASSERT_EQ(buf[sync_start + 11], MFM_PULSE_LONG);
    ASSERT_EQ(buf[sync_start + 12], MFM_PULSE_MEDIUM);
    ASSERT_EQ(buf[sync_start + 13], MFM_PULSE_LONG);
    ASSERT_EQ(buf[sync_start + 14], MFM_PULSE_MEDIUM);
}

// Test: CRC calculation
TEST(test_crc) {
    uint8_t data[] = {0xFE, 0x00, 0x00, 0x01, 0x02};

    // Using shared crc16_mfm
    uint16_t crc = crc16_mfm(data, 5);

    // Manual calculation should match
    uint16_t manual_crc = 0xFFFF;
    manual_crc = crc16_update(manual_crc, 0xA1);
    manual_crc = crc16_update(manual_crc, 0xA1);
    manual_crc = crc16_update(manual_crc, 0xA1);
    for (int i = 0; i < 5; i++) {
        manual_crc = crc16_update(manual_crc, data[i]);
    }

    printf("\n  CRC: %04X, Manual CRC: %04X\n  ", crc, manual_crc);

    ASSERT_EQ(crc, manual_crc);
}

// Test: Decoder finds sync from encoder output
TEST(test_roundtrip_sync) {
    uint8_t buf[1024];
    mfm_encode_t enc;
    mfm_encode_init(&enc, buf, sizeof(buf));

    mfm_encode_sync(&enc);

    // Feed to decoder
    mfm_t mfm;
    sector_t sector;
    mfm_init(&mfm);

    for (size_t i = 0; i < enc.pos; i++) {
        mfm_feed(&mfm, pulse_to_delta(buf[i]), &sector);
    }

    printf("\n  Syncs found: %u\n  ", mfm.syncs_found);

    ASSERT_EQ(mfm.syncs_found, 1);
}

// Test: Full roundtrip - encode address record, decode it
TEST(test_roundtrip_address_record) {
    uint8_t buf[2048];
    mfm_encode_t enc;
    mfm_encode_init(&enc, buf, sizeof(buf));

    // Build address record
    uint8_t addr[5] = {0xFE, 0x05, 0x01, 0x03, 0x02};  // track=5, side=1, sector=3, size=512
    uint16_t crc = crc16_mfm(addr, 5);
    uint8_t crc_bytes[2] = {crc >> 8, crc & 0xFF};

    mfm_encode_sync(&enc);
    mfm_encode_bytes(&enc, addr, 5);
    mfm_encode_bytes(&enc, crc_bytes, 2);

    // Add trailing gap to flush decoder
    mfm_encode_gap(&enc, 4);

    printf("\n  Encoded %zu pulses\n  ", enc.pos);

    // Decode
    mfm_t mfm;
    sector_t sector;
    mfm_init(&mfm);

    for (size_t i = 0; i < enc.pos; i++) {
        mfm_feed(&mfm, pulse_to_delta(buf[i]), &sector);
    }

    printf("  Syncs: %u, CRC errors: %u, have_addr: %d\n  ",
           mfm.syncs_found, mfm.crc_errors, mfm.have_pending_addr);

    ASSERT_EQ(mfm.syncs_found, 1);
    ASSERT_EQ(mfm.crc_errors, 0);
    ASSERT(mfm.have_pending_addr);
    ASSERT_EQ(mfm.pending_track, 5);
    ASSERT_EQ(mfm.pending_side, 1);
    ASSERT_EQ(mfm.pending_sector, 3);
    ASSERT_EQ(mfm.pending_size_code, 2);
}

// Test: Full roundtrip - encode and decode complete sector
TEST(test_roundtrip_full_sector) {
    uint8_t buf[16384];
    mfm_encode_t enc;
    mfm_encode_init(&enc, buf, sizeof(buf));

    // Create test sector
    sector_t src = {.track = 10, .side = 0, .sector_n = 7};
    for (int i = 0; i < 512; i++) {
        src.data[i] = i & 0xFF;
    }

    // Encode complete sector
    mfm_encode_sector(&enc, &src);

    // Add trailing gap
    mfm_encode_gap(&enc, 10);

    printf("\n  Encoded sector: %zu pulses\n  ", enc.pos);

    // Decode
    mfm_t mfm;
    sector_t sector;
    mfm_init(&mfm);

    bool got_sector = false;
    for (size_t i = 0; i < enc.pos; i++) {
        if (mfm_feed(&mfm, pulse_to_delta(buf[i]), &sector)) {
            got_sector = true;
            break;
        }
    }

    printf("  Syncs: %u, CRC errors: %u, got_sector: %d\n  ",
           mfm.syncs_found, mfm.crc_errors, got_sector);

    ASSERT(got_sector);
    ASSERT(sector.valid);
    ASSERT_EQ(sector.track, 10);
    ASSERT_EQ(sector.side, 0);
    ASSERT_EQ(sector.sector_n, 7);
    ASSERT_EQ(sector.size, 512);

    // Verify data
    for (int i = 0; i < 512; i++) {
        if (sector.data[i] != (i & 0xFF)) {
            printf("  Data mismatch at %d: expected %02X, got %02X\n  ",
                   i, i & 0xFF, sector.data[i]);
            ASSERT(0);
        }
    }
}

// Test: Roundtrip with all zeros
TEST(test_roundtrip_all_zeros) {
    uint8_t buf[16384];
    mfm_encode_t enc;
    mfm_encode_init(&enc, buf, sizeof(buf));

    sector_t src = {.track = 0, .side = 0, .sector_n = 1};
    memset(src.data, 0, SECTOR_SIZE);

    mfm_encode_sector(&enc, &src);
    mfm_encode_gap(&enc, 10);

    mfm_t mfm;
    sector_t sector;
    mfm_init(&mfm);

    bool got_sector = false;
    for (size_t i = 0; i < enc.pos; i++) {
        if (mfm_feed(&mfm, pulse_to_delta(buf[i]), &sector)) {
            got_sector = true;
            break;
        }
    }

    printf("\n  Syncs: %u, CRC errors: %u\n  ", mfm.syncs_found, mfm.crc_errors);

    ASSERT(got_sector);
    ASSERT(sector.valid);
    ASSERT_EQ(mfm.crc_errors, 0);

    for (int i = 0; i < 512; i++) {
        ASSERT_EQ(sector.data[i], 0);
    }
}

// Test: Roundtrip with all ones
TEST(test_roundtrip_all_ones) {
    uint8_t buf[16384];
    mfm_encode_t enc;
    mfm_encode_init(&enc, buf, sizeof(buf));

    sector_t src = {.track = 0, .side = 0, .sector_n = 1};
    memset(src.data, 0xFF, SECTOR_SIZE);

    mfm_encode_sector(&enc, &src);
    mfm_encode_gap(&enc, 10);

    mfm_t mfm;
    sector_t sector;
    mfm_init(&mfm);

    bool got_sector = false;
    for (size_t i = 0; i < enc.pos; i++) {
        if (mfm_feed(&mfm, pulse_to_delta(buf[i]), &sector)) {
            got_sector = true;
            break;
        }
    }

    printf("\n  Syncs: %u, CRC errors: %u\n  ", mfm.syncs_found, mfm.crc_errors);

    ASSERT(got_sector);
    ASSERT(sector.valid);

    for (int i = 0; i < 512; i++) {
        ASSERT_EQ(sector.data[i], 0xFF);
    }
}

// Test: Roundtrip with alternating 0xAA
TEST(test_roundtrip_alternating_aa) {
    uint8_t buf[16384];
    mfm_encode_t enc;
    mfm_encode_init(&enc, buf, sizeof(buf));

    sector_t src = {.track = 0, .side = 0, .sector_n = 1};
    memset(src.data, 0xAA, SECTOR_SIZE);

    mfm_encode_sector(&enc, &src);
    mfm_encode_gap(&enc, 10);

    mfm_t mfm;
    sector_t sector;
    mfm_init(&mfm);

    bool got_sector = false;
    for (size_t i = 0; i < enc.pos; i++) {
        if (mfm_feed(&mfm, pulse_to_delta(buf[i]), &sector)) {
            got_sector = true;
            break;
        }
    }

    printf("\n  Syncs: %u, CRC errors: %u\n  ", mfm.syncs_found, mfm.crc_errors);

    ASSERT(got_sector);
    ASSERT(sector.valid);

    for (int i = 0; i < 512; i++) {
        ASSERT_EQ(sector.data[i], 0xAA);
    }
}

// Test: Roundtrip with alternating 0x55
TEST(test_roundtrip_alternating_55) {
    uint8_t buf[16384];
    mfm_encode_t enc;
    mfm_encode_init(&enc, buf, sizeof(buf));

    sector_t src = {.track = 0, .side = 0, .sector_n = 1};
    memset(src.data, 0x55, SECTOR_SIZE);

    mfm_encode_sector(&enc, &src);
    mfm_encode_gap(&enc, 10);

    mfm_t mfm;
    sector_t sector;
    mfm_init(&mfm);

    bool got_sector = false;
    for (size_t i = 0; i < enc.pos; i++) {
        if (mfm_feed(&mfm, pulse_to_delta(buf[i]), &sector)) {
            got_sector = true;
            break;
        }
    }

    printf("\n  Syncs: %u, CRC errors: %u\n  ", mfm.syncs_found, mfm.crc_errors);

    ASSERT(got_sector);
    ASSERT(sector.valid);

    for (int i = 0; i < 512; i++) {
        ASSERT_EQ(sector.data[i], 0x55);
    }
}

// Test: Roundtrip with random data
TEST(test_roundtrip_random) {
    uint8_t buf[16384];
    mfm_encode_t enc;
    mfm_encode_init(&enc, buf, sizeof(buf));

    sector_t src = {.track = 0x10, .side = 1, .sector_n = 5};
    uint32_t seed = 12345;
    for (int i = 0; i < 512; i++) {
        seed = seed * 1103515245 + 12345;
        src.data[i] = (seed >> 16) & 0xFF;
    }

    mfm_encode_sector(&enc, &src);
    mfm_encode_gap(&enc, 10);

    mfm_t mfm;
    sector_t sector;
    mfm_init(&mfm);

    bool got_sector = false;
    for (size_t i = 0; i < enc.pos; i++) {
        if (mfm_feed(&mfm, pulse_to_delta(buf[i]), &sector)) {
            got_sector = true;
            break;
        }
    }

    printf("\n  Syncs: %u, CRC errors: %u\n  ", mfm.syncs_found, mfm.crc_errors);

    ASSERT(got_sector);
    ASSERT(sector.valid);
    ASSERT_EQ(sector.track, 0x10);
    ASSERT_EQ(sector.side, 1);
    ASSERT_EQ(sector.sector_n, 5);

    // Verify data
    seed = 12345;
    for (int i = 0; i < 512; i++) {
        seed = seed * 1103515245 + 12345;
        uint8_t expected = (seed >> 16) & 0xFF;
        if (sector.data[i] != expected) {
            printf("  Mismatch at %d: expected %02X, got %02X\n  ",
                   i, expected, sector.data[i]);
            ASSERT(0);
        }
    }
}

// Test: Multiple sectors in sequence
TEST(test_roundtrip_multiple_sectors) {
    uint8_t buf[65536];
    mfm_encode_t enc;
    mfm_encode_init(&enc, buf, sizeof(buf));

    // Encode 3 sectors
    for (int sec = 1; sec <= 3; sec++) {
        sector_t src = {.track = 0, .side = 0, .sector_n = sec};
        memset(src.data, sec * 0x11, SECTOR_SIZE);
        mfm_encode_sector(&enc, &src);
        mfm_encode_gap(&enc, 54);
    }

    printf("\n  Encoded %zu pulses for 3 sectors\n  ", enc.pos);

    mfm_t mfm;
    sector_t sector;
    mfm_init(&mfm);

    int sectors_found = 0;
    for (size_t i = 0; i < enc.pos; i++) {
        if (mfm_feed(&mfm, pulse_to_delta(buf[i]), &sector)) {
            sectors_found++;
            printf("  Got sector %d (valid=%d, data[0]=%02X)\n  ",
                   sector.sector_n, sector.valid, sector.data[0]);
            ASSERT(sector.valid);
            ASSERT_EQ(sector.data[0], sector.sector_n * 0x11);
        }
    }

    printf("  Found %d sectors, syncs: %u, errors: %u\n  ",
           sectors_found, mfm.syncs_found, mfm.crc_errors);

    ASSERT_EQ(sectors_found, 3);
    ASSERT_EQ(mfm.crc_errors, 0);
}

// Test: Roundtrip with realistic jitter
TEST(test_roundtrip_with_jitter) {
    uint8_t buf[16384];
    mfm_encode_t enc;
    mfm_encode_init(&enc, buf, sizeof(buf));

    sector_t src = {.track = 0, .side = 0, .sector_n = 1};
    for (int i = 0; i < 512; i++) {
        src.data[i] = i & 0xFF;
    }

    mfm_encode_sector(&enc, &src);
    mfm_encode_gap(&enc, 10);

    mfm_t mfm;
    sector_t sector;
    mfm_init(&mfm);

    uint32_t jitter_seed = 54321;
    bool got_sector = false;
    for (size_t i = 0; i < enc.pos; i++) {
        uint16_t delta = pulse_to_delta_jitter(buf[i], &jitter_seed);
        if (mfm_feed(&mfm, delta, &sector)) {
            got_sector = true;
            break;
        }
    }

    printf("\n  Syncs: %u, CRC errors: %u (with jitter)\n  ",
           mfm.syncs_found, mfm.crc_errors);

    ASSERT(got_sector);
    ASSERT(sector.valid);

    for (int i = 0; i < 512; i++) {
        ASSERT_EQ(sector.data[i], i & 0xFF);
    }
}

// Test: Various bit patterns that stress MFM encoding
TEST(test_roundtrip_stress_patterns) {
    uint8_t patterns[][8] = {
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
        {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55},
        {0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF},
        {0xF0, 0x0F, 0xF0, 0x0F, 0xF0, 0x0F, 0xF0, 0x0F},
        {0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01},
        {0x7F, 0xFE, 0x7F, 0xFE, 0x7F, 0xFE, 0x7F, 0xFE},
    };

    for (int p = 0; p < 8; p++) {
        uint8_t buf[16384];
        mfm_encode_t enc;
        mfm_encode_init(&enc, buf, sizeof(buf));

        sector_t src = {.track = 0, .side = 0, .sector_n = 1};
        for (int i = 0; i < 512; i++) {
            src.data[i] = patterns[p][i % 8];
        }

        mfm_encode_sector(&enc, &src);
        mfm_encode_gap(&enc, 10);

        mfm_t mfm;
        sector_t sector;
        mfm_init(&mfm);

        bool got_sector = false;
        for (size_t i = 0; i < enc.pos; i++) {
            if (mfm_feed(&mfm, pulse_to_delta(buf[i]), &sector)) {
                got_sector = true;
                break;
            }
        }

        printf("\n  Pattern %d (%02X %02X...): syncs=%u, errors=%u, valid=%d\n  ",
               p, patterns[p][0], patterns[p][1],
               mfm.syncs_found, mfm.crc_errors, sector.valid);

        ASSERT(got_sector);
        ASSERT(sector.valid);

        for (int i = 0; i < 512; i++) {
            if (sector.data[i] != patterns[p][i % 8]) {
                printf("  Mismatch at %d: expected %02X, got %02X\n  ",
                       i, patterns[p][i % 8], sector.data[i]);
                ASSERT(0);
            }
        }
    }
}

// Test: Full track encode/decode
TEST(test_roundtrip_full_track) {
    uint8_t buf[200000];  // Track can be large
    mfm_encode_t enc;
    mfm_encode_init(&enc, buf, sizeof(buf));

    // Create track with 18 sectors of test data
    track_t trk = {.track = 5, .side = 1};
    for (int s = 0; s < SECTORS_PER_TRACK; s++) {
        trk.sectors[s].track = 5;
        trk.sectors[s].side = 1;
        trk.sectors[s].sector_n = s + 1;
        trk.sectors[s].size = SECTOR_SIZE;
        trk.sectors[s].valid = true;
        for (int i = 0; i < SECTOR_SIZE; i++) {
            trk.sectors[s].data[i] = (s << 4) | (i & 0x0F);
        }
    }

    size_t track_size = mfm_encode_track(&enc, &trk);
    printf("\n  Track size: %zu pulses\n  ", track_size);

    mfm_t mfm;
    sector_t sector;
    mfm_init(&mfm);

    int sectors_found = 0;
    uint8_t found_sectors[18] = {0};

    for (size_t i = 0; i < enc.pos; i++) {
        if (mfm_feed(&mfm, pulse_to_delta(buf[i]), &sector)) {
            if (sector.valid && sector.sector_n >= 1 && sector.sector_n <= 18) {
                found_sectors[sector.sector_n - 1] = 1;
                sectors_found++;

                // Verify this sector's data
                int s = sector.sector_n - 1;
                for (int j = 0; j < 512; j++) {
                    uint8_t expected = (s << 4) | (j & 0x0F);
                    if (sector.data[j] != expected) {
                        printf("  Sector %d byte %d: expected %02X, got %02X\n  ",
                               sector.sector_n, j, expected, sector.data[j]);
                        ASSERT(0);
                    }
                }
            }
        }
    }

    printf("  Found %d/18 sectors, syncs: %u, errors: %u\n  ",
           sectors_found, mfm.syncs_found, mfm.crc_errors);

    ASSERT_EQ(sectors_found, 18);
    ASSERT_EQ(mfm.crc_errors, 0);

    for (int i = 0; i < 18; i++) {
        if (!found_sectors[i]) {
            printf("  Missing sector %d\n  ", i + 1);
            ASSERT(0);
        }
    }
}

int main(void) {
    printf("=== MFM Encoder/Decoder Tests ===\n\n");

    RUN_TEST(test_encoder_basic);
    RUN_TEST(test_encoder_sync);
    RUN_TEST(test_crc);
    RUN_TEST(test_roundtrip_sync);
    RUN_TEST(test_roundtrip_address_record);
    RUN_TEST(test_roundtrip_full_sector);
    RUN_TEST(test_roundtrip_all_zeros);
    RUN_TEST(test_roundtrip_all_ones);
    RUN_TEST(test_roundtrip_alternating_aa);
    RUN_TEST(test_roundtrip_alternating_55);
    RUN_TEST(test_roundtrip_random);
    RUN_TEST(test_roundtrip_multiple_sectors);
    RUN_TEST(test_roundtrip_with_jitter);
    RUN_TEST(test_roundtrip_stress_patterns);
    RUN_TEST(test_roundtrip_full_track);

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
