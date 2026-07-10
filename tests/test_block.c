#include "test.h"
#include "../src/block.h"

TEST(test_geometry_is_bijective) {
  bool seen[DISK_SECTOR_COUNT] = {false};
  for (uint8_t cylinder = 0; cylinder < DISK_CYLINDERS; cylinder++) {
    for (uint8_t head = 0; head < DISK_HEADS; head++) {
      for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
        uint16_t lba = UINT16_MAX;
        ASSERT(disk_chs_to_lba(cylinder, head, sector, &lba));
        ASSERT(lba < DISK_SECTOR_COUNT);
        ASSERT(!seen[lba]);
        seen[lba] = true;
        uint8_t decoded_cylinder = UINT8_MAX;
        uint8_t decoded_head = UINT8_MAX;
        uint8_t decoded_sector = UINT8_MAX;
        ASSERT(disk_lba_to_chs(lba, &decoded_cylinder, &decoded_head,
                               &decoded_sector));
        ASSERT_EQ(decoded_cylinder, cylinder);
        ASSERT_EQ(decoded_head, head);
        ASSERT_EQ(decoded_sector, sector);
      }
    }
  }
  for (uint16_t lba = 0; lba < DISK_SECTOR_COUNT; lba++) ASSERT(seen[lba]);
}

TEST(test_invalid_geometry_preserves_outputs) {
  uint16_t lba = 123u;
  ASSERT(!disk_chs_to_lba(DISK_CYLINDERS, 0, 0, &lba));
  ASSERT_EQ(lba, 123u);
  ASSERT(!disk_chs_to_lba(0, DISK_HEADS, 0, &lba));
  ASSERT_EQ(lba, 123u);
  ASSERT(!disk_chs_to_lba(0, 0, DISK_SECTORS_PER_TRACK, &lba));
  ASSERT_EQ(lba, 123u);
  ASSERT(!disk_chs_to_lba(0, 0, 0, NULL));

  uint8_t cylinder = 11u;
  uint8_t head = 12u;
  uint8_t sector = 13u;
  ASSERT(!disk_lba_to_chs(DISK_SECTOR_COUNT, &cylinder, &head, &sector));
  ASSERT_EQ(cylinder, 11u);
  ASSERT_EQ(head, 12u);
  ASSERT_EQ(sector, 13u);
  ASSERT(!disk_lba_to_chs(0, NULL, &head, &sector));
  ASSERT(!disk_lba_to_chs(0, &cylinder, NULL, &sector));
  ASSERT(!disk_lba_to_chs(0, &cylinder, &head, NULL));
}

TEST(test_track_index_is_bijective) {
  bool seen[DISK_TRACK_COUNT] = {false};
  for (uint8_t cylinder = 0; cylinder < DISK_CYLINDERS; cylinder++) {
    for (uint8_t head = 0; head < DISK_HEADS; head++) {
      uint16_t track = UINT16_MAX;
      ASSERT(disk_ch_to_track(cylinder, head, &track));
      ASSERT(track < DISK_TRACK_COUNT);
      ASSERT(!seen[track]);
      seen[track] = true;
    }
  }
  for (uint16_t track = 0; track < DISK_TRACK_COUNT; track++) {
    ASSERT(seen[track]);
  }
  uint16_t track = 77u;
  ASSERT(!disk_ch_to_track(DISK_CYLINDERS, 0, &track));
  ASSERT_EQ(track, 77u);
  ASSERT(!disk_ch_to_track(0, DISK_HEADS, &track));
  ASSERT_EQ(track, 77u);
  ASSERT(!disk_ch_to_track(0, 0, NULL));
}

TEST(test_track_validity_is_bounded) {
  track_t track = {0};
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    ASSERT(!track_has(&track, sector));
    ASSERT(track_mark(&track, sector));
    ASSERT(track_has(&track, sector));
  }
  ASSERT_EQ(track.valid, DISK_TRACK_VALID);
  uint32_t valid = track.valid;
  ASSERT(!track_mark(&track, DISK_SECTORS_PER_TRACK));
  ASSERT_EQ(track.valid, valid);
  ASSERT(!track_mark(NULL, 0));
  ASSERT(!track_has(NULL, 0));
  ASSERT(!track_has(&track, DISK_SECTORS_PER_TRACK));
}

int main(void) {
  printf("=== Block Contract Tests ===\n\n");
  RUN_TEST(test_geometry_is_bijective);
  RUN_TEST(test_invalid_geometry_preserves_outputs);
  RUN_TEST(test_track_index_is_bijective);
  RUN_TEST(test_track_validity_is_bounded);
  TEST_RESULTS();
}
