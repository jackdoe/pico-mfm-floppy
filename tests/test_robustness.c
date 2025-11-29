#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../src/floppy.h"
#include "../src/crc.h"
#include "../src/mfm_decode.h"
#include "../src/mfm_encode.h"
#include "../src/fat12.h"

// ============== Test Framework ==============
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

// ============== Mock disk for FAT12 tests ==============
#define VDISK_TOTAL_SECTORS (80 * 2 * 18)

typedef struct {
  uint8_t data[VDISK_TOTAL_SECTORS][SECTOR_SIZE];
} vdisk_t;

static inline int vdisk_lba(uint8_t track, uint8_t side, uint8_t sector_n) {
  return (track * 2 + side) * SECTORS_PER_TRACK + (sector_n - 1);
}

bool vdisk_read(void *ctx, sector_t *sector) {
  vdisk_t *disk = (vdisk_t *)ctx;
  int lba = vdisk_lba(sector->track, sector->side, sector->sector_n);
  if (lba < 0 || lba >= VDISK_TOTAL_SECTORS) {
    sector->valid = false;
    return false;
  }
  memcpy(sector->data, disk->data[lba], SECTOR_SIZE);
  sector->valid = true;
  return true;
}

bool vdisk_write(void *ctx, track_t *track) {
  vdisk_t *disk = (vdisk_t *)ctx;
  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    if (!track->sectors[i].valid) {
      int lba = vdisk_lba(track->track, track->side, i + 1);
      if (lba >= 0 && lba < VDISK_TOTAL_SECTORS) {
        memcpy(track->sectors[i].data, disk->data[lba], SECTOR_SIZE);
        track->sectors[i].valid = true;
      }
    }
  }
  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    int lba = vdisk_lba(track->track, track->side, i + 1);
    if (lba >= 0 && lba < VDISK_TOTAL_SECTORS) {
      memcpy(disk->data[lba], track->sectors[i].data, SECTOR_SIZE);
    }
  }
  return true;
}

void vdisk_format_valid(vdisk_t *disk) {
  memset(disk, 0, sizeof(*disk));
  uint8_t *boot = disk->data[0];
  boot[0] = 0xEB; boot[1] = 0x3C; boot[2] = 0x90;
  memcpy(&boot[3], "MSDOS5.0", 8);
  boot[11] = SECTOR_SIZE & 0xFF;
  boot[12] = SECTOR_SIZE >> 8;
  boot[13] = 1;  // sectors per cluster
  boot[14] = 1; boot[15] = 0;  // reserved sectors
  boot[16] = 2;  // num FATs
  boot[17] = 224; boot[18] = 0;  // root entries
  boot[19] = (VDISK_TOTAL_SECTORS) & 0xFF;
  boot[20] = (VDISK_TOTAL_SECTORS) >> 8;
  boot[21] = 0xF0;
  boot[22] = 9; boot[23] = 0;  // sectors per FAT
  boot[24] = 18; boot[25] = 0;  // sectors per track
  boot[26] = 2; boot[27] = 0;   // num heads
  boot[510] = 0x55;
  boot[511] = 0xAA;
  // FAT
  disk->data[1][0] = 0xF0;
  disk->data[1][1] = 0xFF;
  disk->data[1][2] = 0xFF;
  disk->data[10][0] = 0xF0;
  disk->data[10][1] = 0xFF;
  disk->data[10][2] = 0xFF;
}

// ============== FAT12 Robustness Tests ==============

TEST(test_fat12_zero_sectors_per_track) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  // Corrupt: set sectors_per_track to 0
  disk.data[0][24] = 0;
  disk.data[0][25] = 0;

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };

  // Should fail gracefully, not divide by zero
  fat12_err_t err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_ERR_INVALID);
}

TEST(test_fat12_zero_num_heads) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  // Corrupt: set num_heads to 0
  disk.data[0][26] = 0;
  disk.data[0][27] = 0;

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };

  // Should fail gracefully
  fat12_err_t err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_ERR_INVALID);
}

TEST(test_fat12_missing_boot_signature) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  // Corrupt boot signature
  disk.data[0][510] = 0x00;
  disk.data[0][511] = 0x00;

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };

  fat12_err_t err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_ERR_INVALID);
}

TEST(test_fat12_zero_sectors_per_cluster) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  // Corrupt: set sectors_per_cluster to 0
  disk.data[0][13] = 0;

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };

  fat12_err_t err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_ERR_INVALID);
}

TEST(test_fat12_null_read_callback) {
  fat12_t fat;
  fat12_io_t io = { .read = NULL, .write = vdisk_write, .ctx = NULL };

  // Should detect NULL callback
  fat12_err_t err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_ERR_INVALID);
}

TEST(test_fat12_cluster_underflow) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  // Try to read cluster 0 and 1 (invalid, would cause underflow in cluster_to_lba)
  uint8_t buf[512];
  fat12_err_t err = fat12_read_cluster(&fat, 0, buf);
  ASSERT_EQ(err, FAT12_ERR_INVALID);

  err = fat12_read_cluster(&fat, 1, buf);
  ASSERT_EQ(err, FAT12_ERR_INVALID);
}

// ============== MFM Decoder Robustness Tests ==============

// Remap encoder pulse values to decoder-compatible values
static void remap_pulses(uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    // Encoder: SHORT=29, MEDIUM=53, LONG=77
    // Decoder expects: SHORT ~47, MEDIUM ~70, LONG ~95
    if (buf[i] == 29) buf[i] = 47;       // SHORT
    else if (buf[i] == 53) buf[i] = 70;  // MEDIUM
    else if (buf[i] == 77) buf[i] = 95;  // LONG
  }
}

TEST(test_mfm_decode_large_size_code) {
  mfm_t m;
  mfm_init(&m);

  // Build an address record with size_code = 3 (1024 bytes)
  // Decoder should clamp to size_code 2 (512 bytes) to prevent buffer overflow

  uint8_t pulse_buf[8192];
  mfm_encode_t enc;
  mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

  uint8_t addr_data[] = {0xFE, 0x00, 0x00, 0x01, 0x03};  // size_code = 3
  uint16_t crc = 0xFFFF;
  crc = crc16_update(crc, 0xA1);
  crc = crc16_update(crc, 0xA1);
  crc = crc16_update(crc, 0xA1);
  for (int i = 0; i < 5; i++) {
    crc = crc16_update(crc, addr_data[i]);
  }

  // Need gap before sync for preamble detection
  mfm_encode_gap(&enc, 80);

  mfm_encode_sync(&enc);
  mfm_encode_bytes(&enc, addr_data, 5);
  uint8_t crc_bytes[2] = {crc >> 8, crc & 0xFF};
  mfm_encode_bytes(&enc, crc_bytes, 2);

  // Gap between address and data
  mfm_encode_gap(&enc, 22);

  // Send data for 512 bytes (clamped size)
  uint8_t data_mark = 0xFB;
  uint8_t data_buf[512];
  memset(data_buf, 0x42, 512);

  uint16_t data_crc = 0xFFFF;
  data_crc = crc16_update(data_crc, 0xA1);
  data_crc = crc16_update(data_crc, 0xA1);
  data_crc = crc16_update(data_crc, 0xA1);
  data_crc = crc16_update(data_crc, data_mark);
  for (int i = 0; i < 512; i++) {
    data_crc = crc16_update(data_crc, data_buf[i]);
  }

  mfm_encode_sync(&enc);
  mfm_encode_bytes(&enc, &data_mark, 1);
  mfm_encode_bytes(&enc, data_buf, 512);
  uint8_t data_crc_bytes[2] = {data_crc >> 8, data_crc & 0xFF};
  mfm_encode_bytes(&enc, data_crc_bytes, 2);

  // Remap pulse values to decoder-compatible range
  remap_pulses(pulse_buf, enc.pos);

  // Feed to decoder
  sector_t out;
  memset(&out, 0, sizeof(out));
  bool got_sector = false;

  for (size_t i = 0; i < enc.pos; i++) {
    if (mfm_feed(&m, pulse_buf[i], &out)) {
      got_sector = true;
    }
  }

  // Should have gotten a sector - size_code was clamped so 512 bytes matched
  ASSERT(got_sector);
  ASSERT_EQ(out.size_code, 2);  // Clamped from 3 to 2
  ASSERT(out.valid);
}

TEST(test_mfm_decode_invalid_pulses) {
  mfm_t m;
  mfm_init(&m);

  sector_t out;
  memset(&out, 0, sizeof(out));

  // Feed garbage pulse values - should not crash
  uint16_t garbage[] = {0, 1, 5, 10, 20, 30, 150, 200, 255, 1000, 65535};
  for (int i = 0; i < 11; i++) {
    bool got_sector = mfm_feed(&m, garbage[i], &out);
    ASSERT(!got_sector);  // Should not produce a valid sector from garbage
  }

  // Decoder should still be in a valid state
  ASSERT(m.state == MFM_HUNT);
}

TEST(test_mfm_decode_truncated_sector) {
  mfm_t m;
  mfm_init(&m);

  // Create a valid sync + address, but no data
  uint8_t pulse_buf[4096];
  mfm_encode_t enc;
  mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

  sector_t s = {
    .track = 0, .side = 0, .sector_n = 1,
    .size_code = 2, .size = 512, .valid = true
  };
  memset(s.data, 0xAA, 512);

  // Only encode address part, not data
  uint8_t addr[5] = {0xFE, s.track, s.side, s.sector_n, 0x02};
  uint16_t addr_crc = crc16_mfm(addr, 5);

  // Need gap before sync for preamble detection
  mfm_encode_gap(&enc, 80);

  mfm_encode_sync(&enc);
  mfm_encode_bytes(&enc, addr, 5);
  uint8_t addr_crc_bytes[2] = {addr_crc >> 8, addr_crc & 0xFF};
  mfm_encode_bytes(&enc, addr_crc_bytes, 2);

  // Remap pulse values to decoder-compatible range
  remap_pulses(pulse_buf, enc.pos);

  // Feed to decoder - should parse address but not crash waiting for data
  sector_t out;
  for (size_t i = 0; i < enc.pos; i++) {
    mfm_feed(&m, pulse_buf[i], &out);
  }

  // Should have pending address but no complete sector
  ASSERT(m.have_pending_addr == true);
  ASSERT(m.sectors_read == 0);
}

TEST(test_mfm_decode_corrupted_crc) {
  mfm_t m;
  mfm_init(&m);

  uint8_t pulse_buf[8192];
  mfm_encode_t enc;
  mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

  sector_t s = {
    .track = 0, .side = 0, .sector_n = 1,
    .size_code = 2, .size = 512, .valid = true
  };
  memset(s.data, 0x55, 512);

  mfm_encode_sector(&enc, &s);

  // Remap pulse values to decoder-compatible range
  remap_pulses(pulse_buf, enc.pos);

  // Corrupt some pulses in the middle (after remapping)
  if (enc.pos > 100) {
    pulse_buf[50] = 0;  // Invalid pulse
    pulse_buf[51] = 255; // Invalid pulse
  }

  sector_t out;
  memset(&out, 0, sizeof(out));

  for (size_t i = 0; i < enc.pos; i++) {
    mfm_feed(&m, pulse_buf[i], &out);
  }

  // Should either have CRC error or no sector, but not crash
  // CRC errors are expected with corrupted data
  // Just verify we got here without crashing
  (void)m.crc_errors;
}

TEST(test_mfm_encode_buffer_overflow) {
  // Test encoding to a tiny buffer
  uint8_t tiny_buf[10];
  mfm_encode_t enc;
  mfm_encode_init(&enc, tiny_buf, sizeof(tiny_buf));

  // Try to encode a full sector - should not overflow
  sector_t s = {
    .track = 0, .side = 0, .sector_n = 1,
    .size_code = 2, .size = 512, .valid = true
  };
  memset(s.data, 0, 512);

  mfm_encode_sector(&enc, &s);

  // pos should be clamped to size
  ASSERT(enc.pos <= sizeof(tiny_buf));

  // Buffer should not have been overwritten past its size
  // (Can't easily test this without memory sanitizer, but at least verify pos)
}

TEST(test_mfm_decode_rapid_state_changes) {
  mfm_t m;
  mfm_init(&m);

  sector_t out;

  // Simulate rapid sync-like patterns that might confuse state machine
  // Alternating short/medium pulses
  for (int i = 0; i < 1000; i++) {
    uint16_t pulse = (i % 3 == 0) ? 48 : (i % 3 == 1) ? 72 : 96;
    mfm_feed(&m, pulse, &out);
  }

  // Should not crash, state should be valid
  ASSERT(m.state >= MFM_HUNT && m.state <= MFM_CLOCK);
}

TEST(test_mfm_encode_null_sector) {
  uint8_t buf[4096];
  mfm_encode_t enc;
  mfm_encode_init(&enc, buf, sizeof(buf));

  // This test documents that NULL check should be added
  // For now, just verify the encoder handles a zeroed sector
  sector_t s;
  memset(&s, 0, sizeof(s));

  mfm_encode_sector(&enc, &s);

  // Should produce some output without crashing
  ASSERT(enc.pos > 0);
}

// ============== Main ==============

int main(void) {
  printf("=== Robustness Tests ===\n\n");

  printf("--- FAT12 Edge Cases ---\n");
  RUN_TEST(test_fat12_zero_sectors_per_track);
  RUN_TEST(test_fat12_zero_num_heads);
  RUN_TEST(test_fat12_missing_boot_signature);
  RUN_TEST(test_fat12_zero_sectors_per_cluster);
  RUN_TEST(test_fat12_null_read_callback);
  RUN_TEST(test_fat12_cluster_underflow);

  printf("\n--- MFM Decoder Edge Cases ---\n");
  RUN_TEST(test_mfm_decode_large_size_code);
  RUN_TEST(test_mfm_decode_invalid_pulses);
  RUN_TEST(test_mfm_decode_truncated_sector);
  RUN_TEST(test_mfm_decode_corrupted_crc);
  RUN_TEST(test_mfm_encode_buffer_overflow);
  RUN_TEST(test_mfm_decode_rapid_state_changes);
  RUN_TEST(test_mfm_encode_null_sector);

  printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);

  return (tests_passed == tests_run) ? 0 : 1;
}
