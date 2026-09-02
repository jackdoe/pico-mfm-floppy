#include "test.h"
#include "scp_disk.h"
#include "scp_fixture.h"
#include "../src/f12.h"
#include <errno.h>

static uint8_t *fixture_data;
static size_t fixture_size;
static scp_disk_t fixture_disk;
static uint8_t image[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE];
static bool coverage[DISK_SECTOR_COUNT];

static uint32_t checksum(const uint8_t *data, size_t size) {
  uint32_t value = 5381u;
  for (size_t i = 0; i < size; i++) value = value * 33u + data[i];
  return value;
}

static disk_result_t read_all(f12_file_t *file, uint8_t *data, size_t size) {
  disk_result_t total = {.error = DISK_OK, .count = 0};
  while (total.count < size) {
    disk_result_t part = f12_read(file, data + total.count, size - total.count);
    total.count += part.count;
    if (part.error != DISK_OK) {
      total.error = part.error;
      return total;
    }
    if (part.count == 0) {
      total.error = DISK_ERR_IO;
      return total;
    }
  }
  return total;
}

TEST(test_decode_every_sector) {
  memset(image, 0, sizeof(image));
  memset(coverage, 0, sizeof(coverage));
  for (uint8_t cylinder = 0; cylinder < DISK_CYLINDERS; cylinder++) {
    for (uint8_t head = 0; head < DISK_HEADS; head++) {
      track_t track;
      disk_err_t status = scp_disk_read_track(
          &fixture_disk, 1u, cylinder, head, &track);
      ASSERT_EQ(status, DISK_OK);
      ASSERT_EQ(track.cylinder, cylinder);
      ASSERT_EQ(track.head, head);
      ASSERT_EQ(track.valid, DISK_TRACK_VALID);
      for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
        uint16_t lba;
        ASSERT(disk_chs_to_lba(cylinder, head, sector, &lba));
        memcpy(image[lba], track.data[sector], DISK_SECTOR_SIZE);
        coverage[lba] = true;
      }
    }
  }
  for (uint16_t lba = 0; lba < DISK_SECTOR_COUNT; lba++) {
    ASSERT(coverage[lba]);
  }
}

TEST(test_fixed_geometry_fat12_image) {
  const uint8_t *boot = image[0];
  ASSERT_EQ(boot[FAT12_BOOT_SIG_OFFSET], 0x55);
  ASSERT_EQ(boot[FAT12_BOOT_SIG_OFFSET + 1u], 0xAA);
  ASSERT_EQ(boot[21], FAT12_MEDIA_DESCRIPTOR);
  ASSERT_EQ(boot[24], DISK_SECTORS_PER_TRACK);
  ASSERT_EQ(boot[26], DISK_HEADS);
  for (uint16_t sector = 0; sector < FAT12_SECTORS_PER_FAT; sector++) {
    ASSERT_MEM_EQ(image[FAT12_RESERVED_SECTORS + sector],
                  image[FAT12_RESERVED_SECTORS + FAT12_SECTORS_PER_FAT + sector],
                  DISK_SECTOR_SIZE);
  }
  uint32_t value = 0;
  for (uint16_t lba = 0; lba < DISK_SECTOR_COUNT; lba++) {
    value ^= checksum(image[lba], DISK_SECTOR_SIZE);
  }
  ASSERT(value != 0);
}

TEST(test_f12_reads_fixture_consistently) {
  f12_t fs;
  ASSERT_EQ(f12_init(&fs, scp_disk_device(&fixture_disk)), DISK_OK);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);

  fat12_fsck_t report;
  ASSERT_EQ(f12_fsck(&fs, &report, false), DISK_OK);
  ASSERT_EQ(report.crosslinked, 0);
  ASSERT_EQ(report.loops, 0);

  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), DISK_OK);
  size_t files = 0;
  size_t bytes = 0;
  for (;;) {
    f12_stat_t stat;
    disk_err_t error = f12_readdir(&dir, &stat);
    if (error == DISK_END) break;
    ASSERT_EQ(error, DISK_OK);
    if ((stat.attr & FAT12_ATTR_DIRECTORY) != 0 || stat.size == 0) continue;

    uint8_t *first = malloc(stat.size);
    uint8_t *second = malloc(stat.size);
    ASSERT(first != NULL);
    ASSERT(second != NULL);
    f12_file_t file;
    ASSERT_EQ(f12_open(&fs, stat.name, F12_OPEN_READ, &file), DISK_OK);
    disk_result_t result = read_all(&file, first, stat.size);
    ASSERT_EQ(result.error, DISK_OK);
    ASSERT_EQ(result.count, stat.size);
    ASSERT_EQ(f12_close(&file), DISK_OK);
    ASSERT_EQ(f12_open(&fs, stat.name, F12_OPEN_READ, &file), DISK_OK);
    result = read_all(&file, second, stat.size);
    ASSERT_EQ(result.error, DISK_OK);
    ASSERT_EQ(result.count, stat.size);
    ASSERT_EQ(f12_close(&file), DISK_OK);
    ASSERT_MEM_EQ(first, second, stat.size);
    ASSERT_EQ(checksum(first, stat.size), checksum(second, stat.size));
    free(first);
    free(second);
    files++;
    bytes += stat.size;
  }
  ASSERT_EQ(f12_closedir(&dir), DISK_OK);
  ASSERT(files > 0);
  ASSERT(bytes > 0);

  f12_file_t writer;
  ASSERT_EQ(f12_open(&fs, "WRITE.BIN", F12_OPEN_WRITE, &writer),
            DISK_ERR_WRITE_PROTECTED);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

int main(int argc, char **argv) {
  if (argc != 3 || strcmp(argv[1], "--fixture") != 0) {
    fprintf(stderr, "Usage: %s --fixture PATH\n", argv[0]);
    return 2;
  }
  fixture_data = scp_fixture_load(argv[2], &fixture_size);
  if (!fixture_data) {
    fprintf(stderr, "Cannot read required SCP fixture %s: %s\n",
            argv[2], strerror(errno));
    return 1;
  }
  if (!scp_disk_init(&fixture_disk, fixture_data, fixture_size)) {
    fprintf(stderr, "Invalid SCP fixture: %s\n", argv[2]);
    free(fixture_data);
    return 1;
  }

  printf("=== SCP FAT12 Contract Tests ===\n");
  printf("Fixture: %s (%zu bytes)\n\n", argv[2], fixture_size);
  RUN_TEST(test_decode_every_sector);
  RUN_TEST(test_fixed_geometry_fat12_image);
  RUN_TEST(test_f12_reads_fixture_consistently);
  scp_disk_deinit(&fixture_disk);
  free(fixture_data);
  TEST_RESULTS();
}
