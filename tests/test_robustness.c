#include "test.h"
#include "vdisk.h"
#include "../src/crc.h"
#include "../src/mfm.h"

static vdisk_t disk;
static cache_t cache;
static fat12_t fat;

static disk_err_t mount(fat12_t *target, disk_device_t device) {
  ASSERT_EQ(cache_init(&cache, device), DISK_OK);
  ASSERT_EQ(cache_bind(&cache), DISK_OK);
  return fat12_init(target, &cache);
}

static void format_disk(void) {
  vdisk_init(&disk);
  ASSERT_EQ(cache_init(&cache, vdisk_device(&disk)), DISK_OK);
  ASSERT_EQ(cache_bind(&cache), DISK_OK);
  ASSERT_EQ(fat12_format(&fat, &cache, "ROBUST", false, NULL, NULL), DISK_OK);
}

static void set_fat_entry(uint16_t cluster, uint16_t value) {
  vdisk_set_fat_entry(&disk, cluster, value);
}

TEST(test_fat12_rejects_every_noncanonical_bpb_field) {
  static const uint8_t offsets[] = {
      11, 13, 14, 16, 17, 19, 21, 22, 24, 26, 28, 32,
  };
  for (size_t i = 0; i < sizeof(offsets); i++) {
    format_disk();
    disk.data[0][offsets[i]] ^= 1u;
    fat12_t candidate;
    ASSERT_EQ(mount(&candidate, vdisk_device(&disk)), DISK_ERR_INVALID);
  }
}

TEST(test_fat12_rejects_missing_signature_and_flags_bad_fat_markers) {
  format_disk();
  disk.data[0][FAT12_BOOT_SIG_OFFSET] = 0;
  fat12_t candidate;
  ASSERT_EQ(mount(&candidate, vdisk_device(&disk)), DISK_ERR_INVALID);
  format_disk();
  disk.data[FAT12_RESERVED_SECTORS][0] = 0;
  ASSERT_EQ(mount(&candidate, vdisk_device(&disk)), DISK_OK);
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&candidate, &report, false), DISK_OK);
  ASSERT(report.fat_mismatch);
  ASSERT(report.fat_markers_invalid);
  ASSERT_EQ(candidate.fat_start, FAT12_FAT2_START);
  format_disk();
  disk.data[FAT12_FAT1_START][0] = 0;
  disk.data[FAT12_FAT2_START][0] = 0;
  ASSERT_EQ(mount(&candidate, vdisk_device(&disk)), DISK_OK);
  ASSERT_EQ(fat12_fsck(&candidate, &report, false), DISK_OK);
  ASSERT(!report.fat_mismatch);
  ASSERT(report.fat_markers_invalid);
}

TEST(test_fat12_rejects_missing_cache_and_write_protected_devices) {
  fat12_t candidate;
  ASSERT_EQ(fat12_init(&candidate, NULL), DISK_ERR_INVALID);
  ASSERT_EQ(fat12_init(NULL, &cache), DISK_ERR_INVALID);
  format_disk();
  ASSERT_EQ(mount(&candidate, vdisk_readonly_device(&disk)), DISK_OK);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&candidate, "NO.TXT", &writer), DISK_ERR_WRITE_PROTECTED);
  ASSERT_EQ(fat12_delete(&candidate, "NO.TXT"), DISK_ERR_WRITE_PROTECTED);
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&candidate, &report, true), DISK_ERR_WRITE_PROTECTED);
  ASSERT_EQ(fat12_fsck(&candidate, &report, false), DISK_OK);
}

TEST(test_fat12_preserves_typed_read_failure) {
  format_disk();
  disk.fail_track = 0;
  disk.read_failure = DISK_ERR_TIMEOUT;
  fat12_t candidate;
  ASSERT_EQ(mount(&candidate, vdisk_device(&disk)), DISK_ERR_TIMEOUT);
}

TEST(test_fat12_rejects_cluster_underflow) {
  format_disk();
  ASSERT_EQ(mount(&fat, vdisk_device(&disk)), DISK_OK);
  uint16_t next;
  ASSERT_EQ(fat12_get_entry(&fat, 0, &next), DISK_ERR_INVALID);
  ASSERT_EQ(fat12_get_entry(&fat, 1, &next), DISK_ERR_INVALID);
  ASSERT_EQ(fat12_get_entry(&fat, FAT12_CLUSTER_LIMIT, &next), DISK_ERR_INVALID);
  ASSERT_EQ(fat12_get_entry(&fat, 2, NULL), DISK_ERR_INVALID);
  ASSERT_EQ(fat12_get_entry(&fat, 2, &next), DISK_OK);
  ASSERT_EQ(next, 0);
}

TEST(test_fat12_detects_fat_copy_mismatch_on_init_and_fsck) {
  format_disk();
  uint16_t copy = (uint16_t)(FAT12_RESERVED_SECTORS + FAT12_SECTORS_PER_FAT);
  disk.data[copy][3] ^= 0xFFu;
  ASSERT_EQ(mount(&fat, vdisk_device(&disk)), DISK_OK);
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, false), DISK_OK);
  ASSERT(report.fat_mismatch);
}

TEST(test_fat12_fsck_distinguishes_loop) {
  format_disk();
  ASSERT_EQ(mount(&fat, vdisk_device(&disk)), DISK_OK);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "LOOP.TXT", &writer), DISK_OK);
  uint8_t data[DISK_SECTOR_SIZE * 2u];
  memset(data, 0xA5, sizeof(data));
  disk_result_t result = fat12_write(&writer, data, sizeof(data));
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, sizeof(data));
  ASSERT_EQ(fat12_close_write(&writer), DISK_OK);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "LOOP.TXT", &entry), DISK_OK);
  ASSERT(entry.start_cluster >= 2);
  set_fat_entry(entry.start_cluster, entry.start_cluster + 1u);
  set_fat_entry(entry.start_cluster + 1u, entry.start_cluster);
  ASSERT_EQ(mount(&fat, vdisk_device(&disk)), DISK_OK);
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, false), DISK_OK);
  ASSERT_EQ(report.loops, 1);
  ASSERT_EQ(report.broken_chains, 1);
}

static uint16_t pulse_delta(uint8_t pulse) {
  return pulse + MFM_PIO_OVERHEAD;
}

static void encode_address(mfm_encode_t *encoder, uint8_t cylinder, uint8_t head,
                           uint8_t sector, uint8_t size_code, bool corrupt) {
  uint8_t address[] = {MFM_ADDR_MARK, cylinder, head, sector, size_code};
  uint16_t crc = crc16(address, sizeof(address), MFM_CRC_INIT);
  if (corrupt) crc ^= 1u;
  uint8_t crc_bytes[] = {(uint8_t)(crc >> 8), (uint8_t)(crc & 0xFF)};
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
    mfm_feed(&decoder, (uint16_t)(seed >> 16), &sector);
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
  RUN_TEST(test_fat12_rejects_missing_cache_and_write_protected_devices);
  RUN_TEST(test_fat12_preserves_typed_read_failure);
  RUN_TEST(test_fat12_rejects_cluster_underflow);
  RUN_TEST(test_fat12_detects_fat_copy_mismatch_on_init_and_fsck);
  RUN_TEST(test_fat12_fsck_distinguishes_loop);
  RUN_TEST(test_mfm_rejects_non_512_records_and_bad_address_crc);
  RUN_TEST(test_mfm_arbitrary_timing_never_escapes_state_bounds);
  RUN_TEST(test_mfm_truncated_record_never_emits);
  TEST_RESULTS();
}
