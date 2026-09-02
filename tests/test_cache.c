#include "test.h"
#include "vdisk.h"

static vdisk_t disk;
static cache_t cache;

static uint16_t lba_of(uint8_t cylinder, uint8_t head, uint8_t sector) {
  uint16_t lba;
  ASSERT(disk_chs_to_lba(cylinder, head, sector, &lba));
  return lba;
}

static void prepare(void) {
  vdisk_init(&disk);
  for (uint16_t lba = 0; lba < DISK_SECTOR_COUNT; lba++) {
    memset(disk.data[lba], (uint8_t)lba, DISK_SECTOR_SIZE);
  }
  ASSERT_EQ(cache_init(&cache, vdisk_device(&disk)), DISK_OK);
  ASSERT_EQ(cache_bind(&cache), DISK_OK);
}

static bool cache_has(uint8_t cylinder, uint8_t head) {
  for (size_t i = 0; i < CACHE_TRACKS; i++) {
    cache_slot_t *slot = &cache.slots[i];
    if (slot->occupied && slot->track.cylinder == cylinder &&
        slot->track.head == head) {
      return true;
    }
  }
  return false;
}

static cache_slot_t *slot_of(uint8_t cylinder, uint8_t head) {
  for (size_t i = 0; i < CACHE_TRACKS; i++) {
    cache_slot_t *slot = &cache.slots[i];
    if (slot->occupied && slot->track.cylinder == cylinder &&
        slot->track.head == head) {
      return slot;
    }
  }
  return NULL;
}

static void read_track_index(uint16_t track) {
  uint8_t sector[DISK_SECTOR_SIZE];
  ASSERT_EQ(cache_read(&cache, (uint16_t)(track * DISK_SECTORS_PER_TRACK), sector),
            DISK_OK);
}

static void write_track_index(uint16_t track, uint8_t value) {
  uint8_t sector[DISK_SECTOR_SIZE];
  memset(sector, value, sizeof(sector));
  ASSERT_EQ(cache_write(&cache, (uint16_t)(track * DISK_SECTORS_PER_TRACK), sector),
            DISK_OK);
}

TEST(test_init_rejects_incomplete_devices) {
  vdisk_init(&disk);
  disk_device_t device = vdisk_device(&disk);
  device.read_track = NULL;
  ASSERT_EQ(cache_init(&cache, device), DISK_ERR_INVALID);
  device = vdisk_device(&disk);
  device.media_generation = NULL;
  ASSERT_EQ(cache_init(&cache, device), DISK_ERR_INVALID);
  device = vdisk_device(&disk);
  device.write_protected = NULL;
  ASSERT_EQ(cache_init(&cache, device), DISK_ERR_INVALID);
  ASSERT_EQ(cache_init(NULL, vdisk_device(&disk)), DISK_ERR_INVALID);
  ASSERT_EQ(cache_init(&cache, vdisk_readonly_device(&disk)), DISK_OK);
  ASSERT_EQ(cache_read(&cache, DISK_SECTOR_COUNT, NULL), DISK_ERR_INVALID);
}

TEST(test_misses_read_a_track_and_hits_are_free) {
  prepare();
  uint8_t sector[DISK_SECTOR_SIZE];
  ASSERT_EQ(cache_read(&cache, 0, sector), DISK_OK);
  ASSERT_EQ(sector[0], 0);
  ASSERT_EQ(disk.track_reads, 1);
  ASSERT_EQ(cache_read(&cache, 5, sector), DISK_OK);
  ASSERT_EQ(sector[0], 5);
  ASSERT_EQ(disk.track_reads, 1);
  ASSERT_EQ(cache_read(&cache, DISK_SECTORS_PER_TRACK, sector), DISK_OK);
  ASSERT_EQ(sector[0], DISK_SECTORS_PER_TRACK);
  ASSERT_EQ(disk.track_reads, 2);
}

TEST(test_eviction_is_lru) {
  prepare();
  for (uint16_t track = 0; track <= CACHE_TRACKS; track++) read_track_index(track);
  ASSERT_EQ(disk.track_reads, CACHE_TRACKS + 1u);
  ASSERT(!cache_has(0, 0));
  for (uint16_t track = 1; track <= CACHE_TRACKS; track++) {
    ASSERT(cache_has((uint8_t)(track / DISK_HEADS), (uint8_t)(track % DISK_HEADS)));
  }
  read_track_index(1);
  read_track_index(CACHE_TRACKS + 1u);
  ASSERT(cache_has(0, 1));
  ASSERT(!cache_has(1, 0));
}

TEST(test_clean_slots_are_evicted_before_dirty_ones) {
  prepare();
  write_track_index(0, 0xAA);
  for (uint16_t track = 1; track < CACHE_TRACKS; track++) read_track_index(track);
  read_track_index(CACHE_TRACKS);
  ASSERT(cache_has(0, 0));
  ASSERT(!cache_has(0, 1));
  ASSERT_EQ(disk.track_writes, 0);
  ASSERT(cache_dirty(&cache));
}

TEST(test_full_dirty_cache_writes_the_oldest_track) {
  prepare();
  for (uint16_t track = 0; track < CACHE_TRACKS; track++) {
    write_track_index(track, (uint8_t)(0xB0 + track));
  }
  write_track_index(CACHE_TRACKS, 0xC0);
  ASSERT_EQ(disk.track_writes, 1);
  ASSERT_EQ(disk.write_order[0], 0);
  ASSERT_EQ(disk.data[0][0], 0xB0);
  ASSERT_EQ(disk.data[1][0], 1);
  ASSERT(!cache_has(0, 0));
  ASSERT(cache_has((uint8_t)(CACHE_TRACKS / DISK_HEADS),
                   (uint8_t)(CACHE_TRACKS % DISK_HEADS)));
}

TEST(test_flush_writes_dirty_tracks_in_lba_order) {
  prepare();
  write_track_index(3, 0x33);
  write_track_index(1, 0x11);
  write_track_index(2, 0x22);
  ASSERT_EQ(cache_flush(&cache), DISK_OK);
  ASSERT_EQ(disk.track_writes, 3);
  ASSERT_EQ(disk.write_order[0], 1);
  ASSERT_EQ(disk.write_order[1], 2);
  ASSERT_EQ(disk.write_order[2], 3);
  ASSERT(!cache_dirty(&cache));
  ASSERT_EQ(disk.data[DISK_SECTORS_PER_TRACK][0], 0x11);
  ASSERT_EQ(disk.data[DISK_SECTORS_PER_TRACK + 1u][0], DISK_SECTORS_PER_TRACK + 1u);
}

TEST(test_partial_write_materializes_full_track) {
  prepare();
  uint16_t first = lba_of(12, 1, 0);
  uint8_t sector[DISK_SECTOR_SIZE];
  memset(sector, 0xA5, sizeof(sector));
  ASSERT_EQ(cache_write(&cache, (uint16_t)(first + 4u), sector), DISK_OK);
  ASSERT_EQ(disk.track_reads, 0);
  ASSERT_EQ(cache_flush(&cache), DISK_OK);
  ASSERT_EQ(disk.track_reads, 1);
  ASSERT_EQ(disk.last_write.valid, DISK_TRACK_VALID);
  ASSERT_EQ(disk.last_expected_generation, disk.generation);
  ASSERT_EQ(disk.data[first + 4u][0], 0xA5);
  ASSERT_EQ(disk.data[first + 3u][0], (uint8_t)(first + 3u));
  ASSERT(cache_has(12, 1));
  ASSERT_EQ(slot_of(12, 1)->track.valid, DISK_TRACK_VALID);
}

TEST(test_failed_flush_keeps_dirty_data_and_discard_restores_device) {
  prepare();
  uint16_t lba = lba_of(8, 0, 0);
  memset(disk.data[lba], 0x11, DISK_SECTOR_SIZE);
  uint8_t sector[DISK_SECTOR_SIZE];
  memset(sector, 0x22, sizeof(sector));
  ASSERT_EQ(cache_write(&cache, lba, sector), DISK_OK);
  disk.write_status = DISK_ERR_VERIFY;
  ASSERT_EQ(cache_flush(&cache), DISK_ERR_VERIFY);
  ASSERT(cache_dirty(&cache));
  ASSERT_EQ(disk.data[lba][0], 0x11);
  ASSERT_EQ(cache_read(&cache, lba, sector), DISK_OK);
  ASSERT_EQ(sector[0], 0x22);
  disk.write_status = DISK_OK;
  cache_discard(&cache);
  ASSERT(!cache_dirty(&cache));
  ASSERT_EQ(cache_read(&cache, lba, sector), DISK_OK);
  ASSERT_EQ(sector[0], 0x11);
  ASSERT_EQ(cache_write(&cache, lba, sector), DISK_OK);
  ASSERT_EQ(cache_flush(&cache), DISK_OK);
  ASSERT_EQ(disk.track_writes, 1);
}

TEST(test_partial_crc_track_serves_valid_sectors) {
  prepare();
  disk.read_status = DISK_ERR_CRC;
  disk.error_valid = DISK_TRACK_VALID ^ (1u << 17);
  uint16_t lba = lba_of(0, 0, 3);
  uint8_t sector[DISK_SECTOR_SIZE];
  ASSERT_EQ(cache_read(&cache, lba, sector), DISK_OK);
  ASSERT_EQ(disk.track_reads, 1);
  ASSERT_EQ(cache_read(&cache, (uint16_t)(lba + 1u), sector), DISK_OK);
  ASSERT_EQ(disk.track_reads, 1);
  ASSERT_EQ(cache_read(&cache, (uint16_t)(lba + 14u), sector), DISK_ERR_CRC);
  ASSERT_EQ(disk.track_reads, 2);
}

TEST(test_conflicting_partial_reads_never_form_a_track) {
  prepare();
  disk.read_status = DISK_ERR_CRC;
  uint16_t lba = lba_of(9, 0, 3);
  memset(disk.data[lba], 0x31, DISK_SECTOR_SIZE);
  disk.error_valid = 1u << 3;
  uint8_t sector[DISK_SECTOR_SIZE];
  ASSERT_EQ(cache_read(&cache, lba, sector), DISK_OK);
  memset(disk.data[lba], 0x32, DISK_SECTOR_SIZE);
  disk.error_valid = (1u << 3) | (1u << 17);
  ASSERT_EQ(cache_read(&cache, (uint16_t)(lba + 14u), sector), DISK_OK);
  ASSERT_EQ(cache_read(&cache, lba, sector), DISK_ERR_CORRUPT);
  disk.read_status = DISK_OK;
  ASSERT_EQ(cache_read(&cache, lba, sector), DISK_OK);
  ASSERT_EQ(sector[0], 0x32);
  ASSERT_EQ(slot_of(9, 0)->conflicted, 0u);
  ASSERT_EQ(slot_of(9, 0)->track.valid, DISK_TRACK_VALID);
}

TEST(test_media_change_clears_everything_until_rebind) {
  prepare();
  write_track_index(2, 0x55);
  read_track_index(4);
  disk.generation_calls = 0;
  disk.change_generation_on_call = 1;
  uint8_t sector[DISK_SECTOR_SIZE];
  ASSERT_EQ(cache_read(&cache, 0, sector), DISK_ERR_MEDIA_CHANGED);
  ASSERT(!cache_has(1, 0));
  ASSERT(!cache_has(2, 0));
  ASSERT(!cache_dirty(&cache));
  ASSERT_EQ(cache_check(&cache), DISK_ERR_MEDIA_CHANGED);
  ASSERT_EQ(cache_bind(&cache), DISK_OK);
  ASSERT_EQ(cache_read(&cache, 0, sector), DISK_OK);
  ASSERT_EQ(disk.track_writes, 0);
}

TEST(test_write_protection_is_typed) {
  prepare();
  ASSERT_EQ(cache_init(&cache, vdisk_readonly_device(&disk)), DISK_OK);
  ASSERT_EQ(cache_bind(&cache), DISK_OK);
  uint8_t sector[DISK_SECTOR_SIZE] = {0};
  ASSERT_EQ(cache_writable(&cache), DISK_ERR_WRITE_PROTECTED);
  ASSERT_EQ(cache_write(&cache, 0, sector), DISK_ERR_WRITE_PROTECTED);
  ASSERT_EQ(cache_read(&cache, 0, sector), DISK_OK);

  prepare();
  disk.write_protected = true;
  ASSERT_EQ(cache_writable(&cache), DISK_ERR_WRITE_PROTECTED);
  ASSERT_EQ(cache_write(&cache, 0, sector), DISK_OK);
  ASSERT_EQ(cache_flush(&cache), DISK_ERR_WRITE_PROTECTED);
  disk.write_protected_status = DISK_ERR_TIMEOUT;
  ASSERT_EQ(cache_writable(&cache), DISK_ERR_TIMEOUT);
}

TEST(test_clock_wrap_keeps_recent_slots) {
  prepare();
  for (uint16_t track = 0; track < CACHE_TRACKS; track++) read_track_index(track);
  cache.clock = UINT64_MAX;
  read_track_index(0);
  ASSERT_EQ(cache.clock, 1);
  read_track_index(CACHE_TRACKS);
  read_track_index(CACHE_TRACKS + 1u);
  ASSERT(cache_has(0, 0));
  ASSERT_EQ(cache.clock, 3);
}

static uint8_t model[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE];
static uint32_t lcg_state;

static uint32_t lcg(void) {
  lcg_state = lcg_state * 1664525u + 1013904223u;
  return lcg_state >> 8;
}

static void random_sector(uint8_t out[DISK_SECTOR_SIZE]) {
  for (size_t i = 0; i < DISK_SECTOR_SIZE; i++) out[i] = (uint8_t)lcg();
}

static bool model_committed(void) {
  return memcmp(model, disk.data, sizeof(model)) == 0;
}

static void model_write(uint16_t lba) {
  uint8_t sector[DISK_SECTOR_SIZE];
  random_sector(sector);
  ASSERT_EQ(cache_write(&cache, lba, sector), DISK_OK);
  memcpy(model[lba], sector, DISK_SECTOR_SIZE);
}

static void model_read(uint16_t lba) {
  uint8_t sector[DISK_SECTOR_SIZE];
  ASSERT_EQ(cache_read(&cache, lba, sector), DISK_OK);
  ASSERT_MEM_EQ(sector, model[lba], DISK_SECTOR_SIZE);
}

static void model_flush(void) {
  ASSERT_EQ(cache_flush(&cache), DISK_OK);
  ASSERT(!cache_dirty(&cache));
  ASSERT(model_committed());
}

TEST(test_random_operations_match_flat_model) {
  prepare();
  memcpy(model, disk.data, sizeof(model));
  lcg_state = 0x5EED0001u;
  const uint16_t span = 10u * DISK_SECTORS_PER_TRACK;
  uint8_t sector[DISK_SECTOR_SIZE];
  for (int step = 0; step < 20000; step++) {
    uint32_t roll = lcg() % 100u;
    uint16_t lba = (uint16_t)(lcg() % span);
    if (roll < 50) {
      model_read(lba);
    } else if (roll < 85) {
      model_write(lba);
    } else if (roll < 93) {
      model_flush();
    } else if (roll < 97) {
      cache_discard(&cache);
      memcpy(model, disk.data, sizeof(model));
    } else if (roll < 99) {
      if (!cache_dirty(&cache)) model_write(lba);
      disk.writes_before_failure = 0;
      ASSERT_EQ(cache_flush(&cache), DISK_ERR_VERIFY);
      ASSERT(cache_dirty(&cache));
      disk.writes_before_failure = -1;
      for (uint16_t probe = 0; probe < span; probe++) model_read(probe);
      model_flush();
    } else {
      disk.generation++;
      ASSERT_EQ(cache_read(&cache, lba, sector), DISK_ERR_MEDIA_CHANGED);
      ASSERT_EQ(cache_bind(&cache), DISK_OK);
      memcpy(model, disk.data, sizeof(model));
    }
    ASSERT_EQ(cache_dirty(&cache), !model_committed());
  }
}

TEST(test_flush_cannot_complete_a_partial_slot_from_a_bad_device_read) {
  prepare();
  uint16_t lba = lba_of(5, 0, 3);
  uint8_t sector[DISK_SECTOR_SIZE];
  memset(sector, 0x5A, sizeof(sector));
  ASSERT_EQ(cache_write(&cache, lba, sector), DISK_OK);
  disk.read_status = DISK_ERR_CRC;
  disk.error_valid = DISK_TRACK_VALID ^ (1u << 9);
  ASSERT_EQ(cache_flush(&cache), DISK_ERR_CRC);
  ASSERT_EQ(disk.track_writes, 0);
  ASSERT_EQ(slot_of(5, 0)->dirty, 1u << 3);
  ASSERT_EQ(slot_of(5, 0)->track.valid, DISK_TRACK_VALID ^ (1u << 9));
  disk.read_status = DISK_OK;
  disk.track_reads = 0;
  ASSERT_EQ(cache_flush(&cache), DISK_OK);
  ASSERT_EQ(disk.track_reads, 1);
  ASSERT_EQ(disk.track_writes, 1);
  ASSERT_EQ(disk.data[lba][0], 0x5A);
  ASSERT(!cache_dirty(&cache));

  prepare();
  ASSERT_EQ(cache_write(&cache, lba, sector), DISK_OK);
  disk.corrupt_track = true;
  ASSERT_EQ(cache_flush(&cache), DISK_ERR_CORRUPT);
  ASSERT_EQ(disk.track_writes, 0);
  ASSERT_EQ(slot_of(5, 0)->dirty, 1u << 3);
  ASSERT_EQ(slot_of(5, 0)->track.valid, DISK_TRACK_VALID ^ 1u);
  disk.corrupt_track = false;
  disk.track_reads = 0;
  ASSERT_EQ(cache_flush(&cache), DISK_OK);
  ASSERT_EQ(disk.track_reads, 1);
  ASSERT_EQ(disk.track_writes, 1);
  ASSERT(!cache_dirty(&cache));

  prepare();
  ASSERT_EQ(cache_write(&cache, lba, sector), DISK_OK);
  disk.read_status = DISK_ERR_CRC;
  disk.error_valid = DISK_TRACK_VALID | (1u << DISK_SECTORS_PER_TRACK);
  ASSERT_EQ(cache_flush(&cache), DISK_ERR_CRC);
  ASSERT_EQ(disk.track_writes, 0);
  ASSERT_EQ(slot_of(5, 0)->track.valid, 1u << 3);
  ASSERT_EQ(cache_read(&cache, (uint16_t)(lba + 1u), sector), DISK_ERR_CRC);
  ASSERT_EQ(slot_of(5, 0)->track.valid, 1u << 3);
  disk.read_status = DISK_OK;
  ASSERT_EQ(cache_flush(&cache), DISK_OK);
  ASSERT_EQ(disk.track_writes, 1);
}

int main(void) {
  printf("=== Track Cache Tests ===\n\n");
  RUN_TEST(test_init_rejects_incomplete_devices);
  RUN_TEST(test_misses_read_a_track_and_hits_are_free);
  RUN_TEST(test_eviction_is_lru);
  RUN_TEST(test_clean_slots_are_evicted_before_dirty_ones);
  RUN_TEST(test_full_dirty_cache_writes_the_oldest_track);
  RUN_TEST(test_flush_writes_dirty_tracks_in_lba_order);
  RUN_TEST(test_partial_write_materializes_full_track);
  RUN_TEST(test_failed_flush_keeps_dirty_data_and_discard_restores_device);
  RUN_TEST(test_partial_crc_track_serves_valid_sectors);
  RUN_TEST(test_conflicting_partial_reads_never_form_a_track);
  RUN_TEST(test_media_change_clears_everything_until_rebind);
  RUN_TEST(test_write_protection_is_typed);
  RUN_TEST(test_clock_wrap_keeps_recent_slots);
  RUN_TEST(test_random_operations_match_flat_model);
  RUN_TEST(test_flush_cannot_complete_a_partial_slot_from_a_bad_device_read);
  TEST_RESULTS();
}
