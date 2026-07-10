#include "test.h"
#include "../src/crc.h"
#include "../src/fat12.h"
#include "../src/mfm_decode.h"
#include "../src/mfm_encode.h"

typedef struct {
  uint8_t data[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE];
  block_status_t read_failure;
  block_status_t write_failure;
  uint16_t fail_lba;
  bool fail_read;
} robust_disk_t;

static robust_disk_t disk;
static fat12_t fat;

static block_status_t disk_read(void *ctx, uint16_t lba,
                                uint8_t out[DISK_SECTOR_SIZE]) {
  robust_disk_t *d = ctx;
  if (lba >= DISK_SECTOR_COUNT) return BLOCK_ERR_INVALID;
  if (d->fail_read && lba == d->fail_lba) return d->read_failure;
  memcpy(out, d->data[lba], DISK_SECTOR_SIZE);
  return BLOCK_OK;
}

static block_status_t disk_write(void *ctx, const track_t *track) {
  robust_disk_t *d = ctx;
  if (d->write_failure != BLOCK_OK) return d->write_failure;
  if (!track || !disk_ch_valid(track->cylinder, track->head) || track->valid == 0) {
    return BLOCK_ERR_INVALID;
  }
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    if (!track_has(track, sector)) continue;
    uint16_t lba;
    if (!disk_chs_to_lba(track->cylinder, track->head, sector, &lba)) {
      return BLOCK_ERR_INVALID;
    }
    memcpy(d->data[lba], track->data[sector], DISK_SECTOR_SIZE);
  }
  return BLOCK_OK;
}

static fat12_io_t disk_io(void) {
  return (fat12_io_t){.read = disk_read, .write = disk_write, .ctx = &disk};
}

static void format_disk(void) {
  memset(&disk, 0, sizeof(disk));
  memset(&fat, 0, sizeof(fat));
  ASSERT_EQ(fat12_format(&fat, disk_io(), "ROBUST", false, NULL, NULL), FAT12_OK);
}

static void set_fat_entry(uint16_t cluster, uint16_t value) {
  uint32_t offset = cluster + cluster / 2u;
  for (uint8_t copy = 0; copy < FAT12_NUM_FATS; copy++) {
    uint16_t start = FAT12_RESERVED_SECTORS + copy * FAT12_SECTORS_PER_FAT;
    uint8_t *bytes = &disk.data[start][0];
    if (cluster & 1u) {
      bytes[offset] = (uint8_t)((bytes[offset] & 0x0Fu) |
                                ((value & 0x0Fu) << 4u));
      bytes[offset + 1u] = (uint8_t)(value >> 4u);
    } else {
      bytes[offset] = (uint8_t)value;
      bytes[offset + 1u] = (bytes[offset + 1u] & 0xF0u) | ((value >> 8) & 0x0Fu);
    }
  }
}

TEST(test_fat12_rejects_every_noncanonical_bpb_field) {
  static const uint8_t offsets[] = {
      11, 13, 14, 16, 17, 19, 21, 22, 24, 26, 28, 32,
  };
  for (size_t i = 0; i < sizeof(offsets); i++) {
    format_disk();
    disk.data[0][offsets[i]] ^= 1u;
    fat12_t candidate;
    ASSERT_EQ(fat12_init(&candidate, disk_io()), FAT12_ERR_INVALID);
  }
}

TEST(test_fat12_rejects_missing_signature_and_flags_bad_fat_markers) {
  format_disk();
  disk.data[0][FAT12_BOOT_SIG_OFFSET] = 0;
  fat12_t candidate;
  ASSERT_EQ(fat12_init(&candidate, disk_io()), FAT12_ERR_INVALID);
  format_disk();
  disk.data[FAT12_RESERVED_SECTORS][0] = 0;
  ASSERT_EQ(fat12_init(&candidate, disk_io()), FAT12_OK);
  ASSERT(candidate.fat_mismatch);
  ASSERT(candidate.fat_markers_invalid);
  ASSERT_EQ(candidate.fat_start, FAT12_FAT2_START);
  format_disk();
  disk.data[FAT12_FAT1_START][0] = 0;
  disk.data[FAT12_FAT2_START][0] = 0;
  ASSERT_EQ(fat12_init(&candidate, disk_io()), FAT12_OK);
  ASSERT(!candidate.fat_mismatch);
  ASSERT(candidate.fat_markers_invalid);
}

TEST(test_fat12_rejects_missing_callbacks) {
  fat12_t candidate;
  ASSERT_EQ(fat12_init(&candidate, (fat12_io_t){0}), FAT12_ERR_INVALID);
  format_disk();
  fat12_io_t readonly = {.read = disk_read, .ctx = &disk};
  ASSERT_EQ(fat12_init(&candidate, readonly), FAT12_OK);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&candidate, "NO.TXT", &writer), FAT12_ERR_READ_ONLY);
}

TEST(test_fat12_preserves_typed_read_failure) {
  format_disk();
  disk.fail_read = true;
  disk.fail_lba = 0;
  disk.read_failure = BLOCK_ERR_TIMEOUT;
  fat12_t candidate;
  ASSERT_EQ(fat12_init(&candidate, disk_io()), FAT12_ERR_READ);
  ASSERT_EQ(fat12_last_io(&candidate), BLOCK_ERR_TIMEOUT);
}

TEST(test_fat12_rejects_cluster_underflow) {
  format_disk();
  ASSERT_EQ(fat12_init(&fat, disk_io()), FAT12_OK);
  uint8_t data[DISK_SECTOR_SIZE];
  ASSERT_EQ(fat12_read_cluster(&fat, 0, data), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_read_cluster(&fat, 1, data), FAT12_ERR_INVALID);
}

TEST(test_fat12_detects_fat_copy_mismatch_on_init_and_fsck) {
  format_disk();
  uint16_t copy = FAT12_RESERVED_SECTORS + FAT12_SECTORS_PER_FAT;
  disk.data[copy][3] ^= 0xFFu;
  ASSERT_EQ(fat12_init(&fat, disk_io()), FAT12_OK);
  ASSERT(fat.fat_mismatch);
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT(report.fat_mismatch);
}

TEST(test_fat12_fsck_distinguishes_loop) {
  format_disk();
  ASSERT_EQ(fat12_init(&fat, disk_io()), FAT12_OK);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "LOOP.TXT", &writer), FAT12_OK);
  uint8_t data[DISK_SECTOR_SIZE * 2u];
  memset(data, 0xA5, sizeof(data));
  fat12_result_t result = fat12_write(&writer, data, sizeof(data));
  ASSERT_EQ(result.error, FAT12_OK);
  ASSERT_EQ(result.count, sizeof(data));
  ASSERT_EQ(fat12_close_write(&writer), FAT12_OK);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "LOOP.TXT", &entry), FAT12_OK);
  ASSERT(entry.start_cluster >= 2);
  set_fat_entry(entry.start_cluster, entry.start_cluster + 1u);
  set_fat_entry(entry.start_cluster + 1u, entry.start_cluster);
  ASSERT_EQ(fat12_init(&fat, disk_io()), FAT12_OK);
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.loops, 1);
  ASSERT_EQ(report.broken_chains, 1);
}

static uint16_t pulse_delta(uint8_t pulse) {
  return pulse + MFM_PIO_OVERHEAD;
}

static void encode_address(mfm_encode_t *encoder, uint8_t cylinder, uint8_t head,
                           uint8_t sector, uint8_t size_code, bool corrupt) {
  uint8_t address[] = {MFM_ADDR_MARK, cylinder, head, sector, size_code};
  uint16_t crc = crc16_mfm(address, sizeof(address));
  if (corrupt) crc ^= 1u;
  uint8_t crc_bytes[] = {crc >> 8, crc & 0xFF};
  mfm_encode_sync(encoder);
  mfm_encode_bytes(encoder, address, sizeof(address));
  mfm_encode_bytes(encoder, crc_bytes, sizeof(crc_bytes));
  mfm_encode_gap(encoder, 4);
}

TEST(test_mfm_rejects_non_512_records_and_bad_address_crc) {
  for (uint8_t size_code = 0; size_code < 4; size_code++) {
    if (size_code == MFM_SIZE_CODE) continue;
    uint8_t pulses[4096];
    mfm_encode_t encoder;
    mfm_encode_init(&encoder, pulses, sizeof(pulses));
    encode_address(&encoder, 0, 0, 1, size_code, false);
    mfm_t decoder;
    mfm_sector_t sector;
    mfm_init(&decoder);
    for (size_t i = 0; i < encoder.pos; i++) {
      ASSERT(!mfm_feed(&decoder, pulse_delta(pulses[i]), &sector));
    }
    ASSERT(decoder.format_errors > 0);
    ASSERT_EQ(decoder.record_state, MFM_EXPECT_ID);
  }
  uint8_t pulses[4096];
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  encode_address(&encoder, 0, 0, 1, MFM_SIZE_CODE, true);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  for (size_t i = 0; i < encoder.pos; i++) {
    ASSERT(!mfm_feed(&decoder, pulse_delta(pulses[i]), &sector));
  }
  ASSERT_EQ(decoder.crc_errors, 1);
}

TEST(test_mfm_arbitrary_timing_never_escapes_state_bounds) {
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  uint32_t seed = 0xD1CEB00Cu;
  for (uint32_t i = 0; i < 200000u; i++) {
    seed = seed * 1664525u + 1013904223u;
    mfm_feed(&decoder, seed >> 16, &sector);
    ASSERT(decoder.state >= MFM_HUNT && decoder.state <= MFM_CLOCK);
    ASSERT(decoder.record_state >= MFM_EXPECT_ID &&
           decoder.record_state <= MFM_READING_DATA);
    ASSERT(decoder.buf_pos <= sizeof(decoder.buf));
  }
}

TEST(test_mfm_truncated_record_never_emits) {
  uint8_t pulses[16384];
  uint8_t data[DISK_SECTOR_SIZE];
  memset(data, 0x5A, sizeof(data));
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_sector(&encoder, 0, 0, 0, data);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  for (size_t i = 0; i < encoder.pos / 2u; i++) {
    ASSERT(!mfm_feed(&decoder, pulse_delta(pulses[i]), &sector));
  }
  ASSERT_EQ(decoder.sectors_read, 0);
}

int main(void) {
  printf("=== Robustness Tests ===\n\n");
  RUN_TEST(test_fat12_rejects_every_noncanonical_bpb_field);
  RUN_TEST(test_fat12_rejects_missing_signature_and_flags_bad_fat_markers);
  RUN_TEST(test_fat12_rejects_missing_callbacks);
  RUN_TEST(test_fat12_preserves_typed_read_failure);
  RUN_TEST(test_fat12_rejects_cluster_underflow);
  RUN_TEST(test_fat12_detects_fat_copy_mismatch_on_init_and_fsck);
  RUN_TEST(test_fat12_fsck_distinguishes_loop);
  RUN_TEST(test_mfm_rejects_non_512_records_and_bad_address_crc);
  RUN_TEST(test_mfm_arbitrary_timing_never_escapes_state_bounds);
  RUN_TEST(test_mfm_truncated_record_never_emits);
  TEST_RESULTS();
}
