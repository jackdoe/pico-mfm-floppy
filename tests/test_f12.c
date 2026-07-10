#include "test.h"
#include "../src/f12.h"

typedef struct {
  uint8_t data[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE];
  track_t last_write;
  block_status_t read_status;
  block_status_t write_status;
  block_status_t generation_status;
  block_status_t write_protected_status;
  uint32_t generation;
  uint32_t reads;
  uint32_t writes;
  uint32_t last_expected_generation;
  uint32_t generation_calls;
  uint32_t change_generation_on_call;
  bool write_protected;
  bool corrupt_track;
  bool apply_failed_write;
  uint32_t error_valid;
} test_disk_t;

typedef struct {
  void *expected;
  uint32_t calls;
} progress_state_t;

static test_disk_t disk;
static f12_t fs;

static uint16_t checked_lba(uint8_t cylinder, uint8_t head, uint8_t sector) {
  uint16_t lba;
  ASSERT(disk_chs_to_lba(cylinder, head, sector, &lba));
  return lba;
}

static bool filesystem_mounted(void) {
  bool mounted = false;
  ASSERT_EQ(f12_is_mounted(&fs, &mounted), F12_OK);
  return mounted;
}

static block_status_t disk_read_track(void *ctx, uint32_t expected_generation,
                                      uint8_t cylinder, uint8_t head,
                                      track_t *out) {
  test_disk_t *device = (test_disk_t *)ctx;
  if (!out || !disk_ch_valid(cylinder, head)) return BLOCK_ERR_INVALID;
  if (expected_generation != device->generation) return BLOCK_ERR_MEDIA_CHANGED;
  memset(out, 0, sizeof(*out));
  out->cylinder = cylinder;
  out->head = head;
  out->valid = device->read_status == BLOCK_OK
      ? (device->corrupt_track ? DISK_TRACK_VALID ^ 1u : DISK_TRACK_VALID)
      : device->error_valid;
  uint16_t first = checked_lba(cylinder, head, 0);
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    memcpy(out->data[sector], device->data[first + sector], DISK_SECTOR_SIZE);
  }
  device->reads++;
  return device->read_status;
}

static block_status_t disk_write_track(void *ctx, uint32_t expected_generation,
                                       const track_t *track) {
  test_disk_t *device = (test_disk_t *)ctx;
  if (!track || !disk_ch_valid(track->cylinder, track->head) ||
      track->valid != DISK_TRACK_VALID) {
    return BLOCK_ERR_INVALID;
  }
  device->last_expected_generation = expected_generation;
  if (expected_generation != device->generation) return BLOCK_ERR_MEDIA_CHANGED;
  if (device->write_protected) return BLOCK_ERR_WRITE_PROTECTED;
  uint16_t first = checked_lba(track->cylinder, track->head, 0);
  if (device->write_status == BLOCK_OK || device->apply_failed_write) {
    for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
      memcpy(device->data[first + sector], track->data[sector], DISK_SECTOR_SIZE);
    }
  }
  if (device->write_status != BLOCK_OK) return device->write_status;
  device->last_write = *track;
  device->writes++;
  return BLOCK_OK;
}

static block_status_t disk_generation(void *ctx, uint32_t *generation) {
  test_disk_t *device = ctx;
  if (!device || !generation) return BLOCK_ERR_INVALID;
  device->generation_calls++;
  if (device->generation_status != BLOCK_OK) {
    return device->generation_status;
  }
  if (device->change_generation_on_call == device->generation_calls) {
    device->generation++;
  }
  *generation = device->generation;
  return BLOCK_OK;
}

static block_status_t disk_write_protected(void *ctx, bool *write_protected) {
  test_disk_t *device = ctx;
  if (!device || !write_protected) return BLOCK_ERR_INVALID;
  if (device->write_protected_status != BLOCK_OK) {
    return device->write_protected_status;
  }
  *write_protected = device->write_protected;
  return BLOCK_OK;
}

static block_device_t disk_device(bool writable) {
  return (block_device_t){
      .read_track = disk_read_track,
      .write_track = writable ? disk_write_track : NULL,
      .media_generation = disk_generation,
      .write_protected = disk_write_protected,
      .ctx = &disk,
  };
}

static void progress(void *ctx, uint8_t cylinder, uint8_t head,
                     uint16_t done, uint16_t total) {
  progress_state_t *state = (progress_state_t *)ctx;
  ASSERT(state == state->expected);
  ASSERT(cylinder < DISK_CYLINDERS);
  ASSERT(head < DISK_HEADS);
  ASSERT(done <= total);
  state->calls++;
}

static void disk_reset(void) {
  memset(&disk, 0, sizeof(disk));
  disk.generation = 1;
}

static f12_format_options_t format_options(const char *label,
                                           f12_format_mode_t mode) {
  return (f12_format_options_t){.label = label, .mode = mode};
}

static void prepare(void) {
  disk_reset();
  ASSERT_EQ(f12_init(&fs, disk_device(true)), F12_OK);
  ASSERT_EQ(f12_format(&fs, format_options("TEST", F12_FORMAT_QUICK)), F12_OK);
  ASSERT_EQ(f12_mount(&fs), F12_OK);
}

static void write_file(const char *name, const void *data, size_t size) {
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, name, F12_OPEN_WRITE, &file), F12_OK);
  if (size != 0) {
    f12_result_t result = f12_write(&file, data, size);
    ASSERT_EQ(result.error, F12_OK);
    ASSERT_EQ(result.count, size);
  }
  ASSERT_EQ(f12_close(&file), F12_OK);
}

static size_t read_file(const char *name, void *data, size_t capacity) {
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, name, F12_OPEN_READ, &file), F12_OK);
  f12_result_t result = f12_read(&file, data, capacity);
  ASSERT(result.error == F12_OK || result.error == F12_END);
  ASSERT_EQ(f12_close(&file), F12_OK);
  return result.count;
}

static bool cache_has(uint8_t cylinder, uint8_t head) {
  for (size_t i = 0; i < F12_CACHE_TRACKS; i++) {
    if (fs.cache[i].occupied &&
        fs.cache[i].track.cylinder == cylinder &&
        fs.cache[i].track.head == head) {
      return true;
    }
  }
  return false;
}

TEST(test_init_contract) {
  disk_reset();
  f12_t automatic;
  ASSERT_EQ(f12_init(&automatic, disk_device(true)), F12_OK);
  bool mounted = true;
  ASSERT_EQ(f12_is_mounted(&automatic, &mounted), F12_OK);
  ASSERT(!mounted);
  block_device_t device = disk_device(true);
  ASSERT_EQ(f12_init(NULL, device), F12_ERR_INVALID);
  device.read_track = NULL;
  ASSERT_EQ(f12_init(&fs, device), F12_ERR_INVALID);
  device = disk_device(true);
  device.media_generation = NULL;
  ASSERT_EQ(f12_init(&fs, device), F12_ERR_INVALID);
  device = disk_device(true);
  device.write_protected = NULL;
  ASSERT_EQ(f12_init(&fs, device), F12_ERR_INVALID);
  ASSERT_EQ(f12_init(&fs, disk_device(false)), F12_OK);
}

TEST(test_format_mount_unmount) {
  prepare();
  ASSERT(filesystem_mounted());
  ASSERT_EQ(f12_mount(&fs), F12_ERR_ALREADY_MOUNTED);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
  ASSERT(!filesystem_mounted());
  ASSERT_EQ(f12_unmount(&fs), F12_ERR_NOT_MOUNTED);
  ASSERT_EQ(f12_mount(&fs), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_format_options_and_progress_context) {
  disk_reset();
  ASSERT_EQ(f12_init(&fs, disk_device(true)), F12_OK);
  progress_state_t state = {.expected = &state};
  f12_format_options_t options = {
      .label = "FULL",
      .mode = F12_FORMAT_FULL,
      .progress = progress,
      .progress_ctx = &state,
  };
  ASSERT_EQ(f12_format(&fs, options), F12_OK);
  ASSERT_EQ(state.calls, DISK_TRACK_COUNT);
  options.mode = (f12_format_mode_t)99;
  ASSERT_EQ(f12_format(&fs, options), F12_ERR_INVALID);
  options.mode = F12_FORMAT_QUICK;
  options.label = NULL;
  ASSERT_EQ(f12_format(&fs, options), F12_ERR_INVALID);
}

TEST(test_read_only_device_mounts) {
  prepare();
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
  ASSERT_EQ(f12_init(&fs, disk_device(false)), F12_OK);
  ASSERT_EQ(f12_mount(&fs), F12_OK);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "NEW.TXT", F12_OPEN_WRITE, &file),
            F12_ERR_WRITE_PROTECTED);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_block_adapter_rejects_null_context_and_output) {
  prepare();
  uint8_t sector[DISK_SECTOR_SIZE];
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, 0, NULL), BLOCK_ERR_INVALID);
  ASSERT_EQ(fs.fat.io.read(NULL, 0, sector), BLOCK_ERR_INVALID);
  ASSERT(filesystem_mounted());
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_track_cache_is_atomic_lru) {
  prepare();
  fs.cache_clock = 0;
  memset(fs.cache, 0, sizeof(fs.cache));
  disk.reads = 0;
  uint8_t sector[DISK_SECTOR_SIZE];
  for (uint8_t track = 0; track < F12_CACHE_TRACKS + 1u; track++) {
    uint16_t lba = (uint16_t)track * DISK_SECTORS_PER_TRACK;
    ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, lba, sector), BLOCK_OK);
  }
  ASSERT_EQ(disk.reads, F12_CACHE_TRACKS + 1u);
  ASSERT(!cache_has(0, 0));
  for (uint8_t track = 1; track < F12_CACHE_TRACKS + 1u; track++) {
    ASSERT(cache_has(track / DISK_HEADS, track % DISK_HEADS));
  }
  for (size_t i = 0; i < F12_CACHE_TRACKS; i++) {
    ASSERT_EQ(fs.cache[i].track.valid, DISK_TRACK_VALID);
  }
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx,
                           F12_CACHE_TRACKS * DISK_SECTORS_PER_TRACK + 7u,
                           sector), BLOCK_OK);
  ASSERT_EQ(disk.reads, F12_CACHE_TRACKS + 1u);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_partial_crc_track_serves_valid_sector) {
  prepare();
  memset(fs.cache, 0, sizeof(fs.cache));
  fs.cache_clock = 0;
  disk.reads = 0;
  disk.read_status = BLOCK_ERR_CRC;
  disk.error_valid = DISK_TRACK_VALID ^ (1u << 17);
  uint16_t lba = checked_lba(0, 0, 3);
  uint8_t sector[DISK_SECTOR_SIZE];
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, lba, sector), BLOCK_OK);
  ASSERT_EQ(disk.reads, 1);
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, lba + 1u, sector), BLOCK_OK);
  ASSERT_EQ(disk.reads, 1);
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, lba + 14u, sector), BLOCK_ERR_CRC);
  ASSERT_EQ(disk.reads, 2);
  disk.read_status = BLOCK_OK;
  disk.error_valid = 0;
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_corrupt_full_track_rejected) {
  disk_reset();
  ASSERT_EQ(f12_init(&fs, disk_device(true)), F12_OK);
  disk.corrupt_track = true;
  ASSERT_EQ(f12_mount(&fs), F12_ERR_CORRUPT);
}

TEST(test_partial_write_materializes_full_track) {
  prepare();
  uint8_t cylinder = 12;
  uint8_t head = 1;
  uint16_t first = checked_lba(cylinder, head, 0);
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    memset(disk.data[first + sector], sector, DISK_SECTOR_SIZE);
  }
  track_t partial = {.cylinder = cylinder, .head = head, .valid = 1u << 4};
  memset(partial.data[4], 0xA5, DISK_SECTOR_SIZE);
  ASSERT_EQ(fs.fat.io.write(fs.fat.io.ctx, &partial), BLOCK_OK);
  ASSERT_EQ(disk.last_write.valid, DISK_TRACK_VALID);
  ASSERT_EQ(disk.last_expected_generation, disk.generation);
  ASSERT_EQ(disk.data[first + 4][0], 0xA5);
  ASSERT_EQ(disk.data[first + 3][0], 3);
  ASSERT(cache_has(cylinder, head));
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_failed_write_does_not_poison_cache) {
  prepare();
  uint8_t data[DISK_SECTOR_SIZE];
  uint8_t cylinder = 8;
  uint8_t head = 0;
  uint16_t lba = checked_lba(cylinder, head, 0);
  memset(disk.data[lba], 0x11, DISK_SECTOR_SIZE);
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, lba, data), BLOCK_OK);
  track_t partial = {.cylinder = cylinder, .head = head, .valid = 1u};
  memset(partial.data[0], 0x22, DISK_SECTOR_SIZE);
  disk.write_status = BLOCK_ERR_VERIFY;
  ASSERT_EQ(fs.fat.io.write(fs.fat.io.ctx, &partial), BLOCK_ERR_VERIFY);
  disk.write_status = BLOCK_OK;
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, lba, data), BLOCK_OK);
  ASSERT_EQ(data[0], 0x11);
  ASSERT_EQ(disk.data[lba][0], 0x11);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_failed_mutating_write_evicts_cache) {
  prepare();
  uint8_t data[DISK_SECTOR_SIZE];
  uint8_t cylinder = 8;
  uint8_t head = 1;
  uint16_t lba = checked_lba(cylinder, head, 0);
  memset(disk.data[lba], 0x11, DISK_SECTOR_SIZE);
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, lba, data), BLOCK_OK);
  track_t partial = {.cylinder = cylinder, .head = head, .valid = 1u};
  memset(partial.data[0], 0x22, DISK_SECTOR_SIZE);
  disk.apply_failed_write = true;
  disk.write_status = BLOCK_ERR_VERIFY;
  ASSERT_EQ(fs.fat.io.write(fs.fat.io.ctx, &partial), BLOCK_ERR_VERIFY);
  uint32_t reads = disk.reads;
  disk.write_status = BLOCK_OK;
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, lba, data), BLOCK_OK);
  ASSERT_EQ(disk.reads, reads + 1u);
  ASSERT_EQ(data[0], 0x22);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_conflicting_partial_reads_never_form_a_track) {
  prepare();
  memset(fs.cache, 0, sizeof(fs.cache));
  fs.cache_clock = 0;
  disk.read_status = BLOCK_ERR_CRC;
  uint16_t lba = checked_lba(9, 0, 3);
  memset(disk.data[lba], 0x31, DISK_SECTOR_SIZE);
  disk.error_valid = 1u << 3;
  uint8_t data[DISK_SECTOR_SIZE];
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, lba, data), BLOCK_OK);
  memset(disk.data[lba], 0x32, DISK_SECTOR_SIZE);
  disk.error_valid = (1u << 3) | (1u << 17);
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, lba + 14u, data), BLOCK_OK);
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, lba, data), BLOCK_ERR_CORRUPT);
  disk.read_status = BLOCK_OK;
  disk.error_valid = 0;
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_file_roundtrip_and_end) {
  prepare();
  uint8_t source[1500];
  uint8_t target[1500];
  for (size_t i = 0; i < sizeof(source); i++) source[i] = (uint8_t)(i * 13u);
  write_file("ROUND.BIN", source, sizeof(source));
  ASSERT_EQ(read_file("ROUND.BIN", target, sizeof(target)), sizeof(target));
  ASSERT_MEM_EQ(source, target, sizeof(source));
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "ROUND.BIN", F12_OPEN_READ, &file), F12_OK);
  f12_result_t result = f12_read(&file, target, sizeof(target));
  ASSERT_EQ(result.error, F12_OK);
  ASSERT_EQ(result.count, sizeof(target));
  result = f12_read(&file, target, 1);
  ASSERT_EQ(result.error, F12_END);
  ASSERT_EQ(result.count, 0);
  ASSERT_EQ(f12_close(&file), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_seek_tell_and_read_at) {
  prepare();
  uint8_t source[1024];
  for (size_t i = 0; i < sizeof(source); i++) source[i] = (uint8_t)i;
  write_file("SEEK.BIN", source, sizeof(source));
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "SEEK.BIN", F12_OPEN_READ, &file), F12_OK);
  ASSERT_EQ(f12_seek(&file, 700), F12_OK);
  uint32_t offset;
  ASSERT_EQ(f12_tell(&file, &offset), F12_OK);
  ASSERT_EQ(offset, 700);
  uint8_t data[20];
  f12_result_t result = f12_read_at(&file, 100, data, sizeof(data));
  ASSERT_EQ(result.error, F12_OK);
  ASSERT_EQ(result.count, sizeof(data));
  ASSERT_MEM_EQ(data, source + 100, sizeof(data));
  ASSERT_EQ(f12_tell(&file, &offset), F12_OK);
  ASSERT_EQ(offset, 700);
  memset(fs.cache, 0, sizeof(fs.cache));
  fs.cache_clock = 0;
  disk.read_status = BLOCK_ERR_TIMEOUT;
  result = f12_read_at(&file, 600, data, sizeof(data));
  ASSERT_EQ(result.error, F12_ERR_TIMEOUT);
  ASSERT_EQ(result.count, 0);
  ASSERT_EQ(f12_tell(&file, &offset), F12_OK);
  ASSERT_EQ(offset, 700);
  disk.read_status = BLOCK_OK;
  ASSERT_EQ(f12_close(&file), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_mode_errors_are_typed) {
  prepare();
  write_file("MODE.BIN", "x", 1);
  f12_file_t reader;
  ASSERT_EQ(f12_open(&fs, "MODE.BIN", F12_OPEN_READ, &reader), F12_OK);
  ASSERT_EQ(f12_write(&reader, "x", 1).error, F12_ERR_CONFLICT);
  ASSERT_EQ(f12_close(&reader), F12_OK);
  f12_file_t writer;
  ASSERT_EQ(f12_open(&fs, "MODE.BIN", F12_OPEN_WRITE, &writer), F12_OK);
  uint8_t byte;
  ASSERT_EQ(f12_read(&writer, &byte, 1).error, F12_ERR_CONFLICT);
  ASSERT_EQ(f12_seek(&writer, 0), F12_ERR_CONFLICT);
  ASSERT_EQ(f12_abort(&writer), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_stale_file_handle_generation) {
  prepare();
  write_file("ONE.BIN", "1", 1);
  write_file("TWO.BIN", "2", 1);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "ONE.BIN", F12_OPEN_READ, &file), F12_OK);
  f12_file_t stale = file;
  ASSERT_EQ(f12_close(&file), F12_OK);
  ASSERT_EQ(f12_open(&fs, "TWO.BIN", F12_OPEN_READ, &file), F12_OK);
  uint8_t byte;
  ASSERT_EQ(f12_read(&stale, &byte, 1).error, F12_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_close(&stale), F12_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_close(&file), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_stale_mount_generation) {
  prepare();
  write_file("STALE.BIN", "x", 1);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "STALE.BIN", F12_OPEN_READ, &file), F12_OK);
  f12_file_t stale = file;
  ASSERT_EQ(f12_unmount(&fs), F12_ERR_CONFLICT);
  ASSERT_EQ(f12_close(&file), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
  ASSERT_EQ(f12_mount(&fs), F12_OK);
  uint8_t byte;
  ASSERT_EQ(f12_read(&stale, &byte, 1).error, F12_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_close(&stale), F12_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_media_generation_invalidates_mount) {
  prepare();
  write_file("MEDIA.BIN", "x", 1);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "MEDIA.BIN", F12_OPEN_READ, &file), F12_OK);
  disk.generation++;
  uint8_t byte;
  ASSERT_EQ(f12_read(&file, &byte, 1).error, F12_ERR_MEDIA_CHANGED);
  ASSERT(!filesystem_mounted());
  ASSERT_EQ(f12_close(&file), F12_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_mount(&fs), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_tell_observes_media_generation) {
  prepare();
  write_file("TELL.BIN", "x", 1);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "TELL.BIN", F12_OPEN_READ, &file), F12_OK);
  disk.generation++;
  uint32_t offset;
  ASSERT_EQ(f12_tell(&file, &offset), F12_ERR_MEDIA_CHANGED);
  ASSERT(!filesystem_mounted());
  ASSERT_EQ(f12_close(&file), F12_ERR_BAD_HANDLE);
}

TEST(test_cache_hit_generation_race_is_detected) {
  prepare();
  uint8_t data[DISK_SECTOR_SIZE];
  uint16_t lba = checked_lba(7, 0, 0);
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, lba, data), BLOCK_OK);
  disk.generation_calls = 0;
  disk.change_generation_on_call = 2;
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, lba, data), BLOCK_ERR_MEDIA_CHANGED);
  disk.change_generation_on_call = 0;
  ASSERT_EQ(f12_unmount(&fs), F12_ERR_MEDIA_CHANGED);
}

TEST(test_reinit_does_not_resurrect_handles) {
  prepare();
  write_file("ABA.BIN", "a", 1);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "ABA.BIN", F12_OPEN_READ, &file), F12_OK);
  f12_file_t stale = file;
  ASSERT_EQ(f12_close(&file), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
  ASSERT_EQ(f12_init(&fs, disk_device(true)), F12_OK);
  ASSERT_EQ(f12_mount(&fs), F12_OK);
  ASSERT_EQ(f12_open(&fs, "ABA.BIN", F12_OPEN_READ, &file), F12_OK);
  uint8_t value;
  ASSERT_EQ(f12_read(&stale, &value, 1).error, F12_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_close(&file), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_live_reinit_resets_and_invalidates_handles) {
  prepare();
  write_file("LIVE.BIN", "x", 1);
  f12_file_t file;
  f12_dir_t dir;
  ASSERT_EQ(f12_open(&fs, "LIVE.BIN", F12_OPEN_READ, &file), F12_OK);
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), F12_OK);
  ASSERT_EQ(f12_init(&fs, disk_device(true)), F12_OK);
  ASSERT(!filesystem_mounted());
  uint8_t byte;
  f12_stat_t stat;
  ASSERT_EQ(f12_read(&file, &byte, 1).error, F12_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_readdir(&dir, &stat), F12_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_mount(&fs), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_invalid_reinit_preserves_live_context) {
  prepare();
  write_file("PRESERVE.BIN", "p", 1);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "PRESERVE.BIN", F12_OPEN_READ, &file), F12_OK);
  block_device_t invalid = disk_device(true);
  invalid.media_generation = NULL;
  ASSERT_EQ(f12_init(&fs, invalid), F12_ERR_INVALID);
  ASSERT(filesystem_mounted());
  uint8_t value = 0;
  ASSERT_EQ(f12_read(&file, &value, 1).error, F12_OK);
  ASSERT_EQ(value, 'p');
  ASSERT_EQ(f12_close(&file), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_media_change_forgets_commit_phase_writer) {
  prepare();
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "SWAP.BIN", F12_OPEN_WRITE, &file), F12_OK);
  ASSERT_EQ(f12_write(&file, "swap", 4).error, F12_OK);
  disk.write_status = BLOCK_ERR_VERIFY;
  ASSERT_EQ(f12_close(&file), F12_ERR_VERIFY);
  disk.write_status = BLOCK_OK;
  disk.generation++;
  ASSERT_EQ(f12_close(&file), F12_ERR_MEDIA_CHANGED);
  ASSERT_EQ(f12_close(&file), F12_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_mount(&fs), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_open_conflicts) {
  prepare();
  write_file("A.BIN", "a", 1);
  write_file("B.BIN", "b", 1);
  f12_file_t reader;
  f12_file_t writer;
  f12_file_t other;
  ASSERT_EQ(f12_open(&fs, "A.BIN", F12_OPEN_READ, &reader), F12_OK);
  ASSERT_EQ(f12_open(&fs, "A.BIN", F12_OPEN_WRITE, &writer), F12_ERR_CONFLICT);
  ASSERT_EQ(f12_open(&fs, "B.BIN", F12_OPEN_WRITE, &writer), F12_OK);
  ASSERT_EQ(f12_open(&fs, "B.BIN", F12_OPEN_READ, &other), F12_ERR_CONFLICT);
  ASSERT_EQ(f12_open(&fs, "C.BIN", F12_OPEN_WRITE, &other), F12_ERR_CONFLICT);
  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), F12_ERR_CONFLICT);
  ASSERT_EQ(f12_delete(&fs, "A.BIN"), F12_ERR_CONFLICT);
  ASSERT_EQ(f12_rename(&fs, "A.BIN", "C.BIN"), F12_ERR_CONFLICT);
  ASSERT_EQ(f12_close(&reader), F12_OK);
  ASSERT_EQ(f12_close(&writer), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_multiple_readers_and_limit) {
  prepare();
  write_file("READ.BIN", "x", 1);
  f12_file_t files[F12_MAX_OPEN_FILES];
  for (size_t i = 0; i < F12_MAX_OPEN_FILES; i++) {
    ASSERT_EQ(f12_open(&fs, "READ.BIN", F12_OPEN_READ, &files[i]), F12_OK);
  }
  f12_file_t extra;
  ASSERT_EQ(f12_open(&fs, "READ.BIN", F12_OPEN_READ, &extra), F12_ERR_TOO_MANY);
  for (size_t i = 0; i < F12_MAX_OPEN_FILES; i++) {
    ASSERT_EQ(f12_close(&files[i]), F12_OK);
  }
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_directory_end_and_generation) {
  prepare();
  write_file("DIR.BIN", "x", 1);
  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), F12_OK);
  f12_dir_t stale = dir;
  f12_stat_t stat;
  ASSERT_EQ(f12_readdir(&dir, &stat), F12_OK);
  ASSERT_STR_EQ(stat.name, "DIR.BIN");
  ASSERT_EQ(f12_readdir(&dir, &stat), F12_END);
  ASSERT_EQ(f12_closedir(&dir), F12_OK);
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), F12_OK);
  ASSERT_EQ(f12_readdir(&stale, &stat), F12_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_closedir(&stale), F12_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_closedir(&dir), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_readdir_preserves_io_error) {
  prepare();
  write_file("IO.BIN", "x", 1);
  memset(fs.cache, 0, sizeof(fs.cache));
  fs.cache_clock = 0;
  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), F12_OK);
  disk.read_status = BLOCK_ERR_TIMEOUT;
  f12_stat_t stat;
  ASSERT_EQ(f12_readdir(&dir, &stat), F12_ERR_TIMEOUT);
  disk.read_status = BLOCK_OK;
  ASSERT_EQ(f12_closedir(&dir), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

static f12_err_t list_fail(void *ctx, const f12_stat_t *stat) {
  size_t *calls = (size_t *)ctx;
  ASSERT(stat != NULL);
  (*calls)++;
  return F12_ERR_CONFLICT;
}

TEST(test_list_propagates_callback_error) {
  prepare();
  write_file("LIST.BIN", "x", 1);
  size_t calls = 0;
  ASSERT_EQ(f12_list(&fs, list_fail, &calls), F12_ERR_CONFLICT);
  ASSERT_EQ(calls, 1);
  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), F12_OK);
  ASSERT_EQ(f12_closedir(&dir), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_delete_rename_and_stat) {
  prepare();
  write_file("OLD.BIN", "data", 4);
  f12_stat_t stat;
  ASSERT_EQ(f12_stat(&fs, "OLD.BIN", &stat), F12_OK);
  ASSERT_EQ(stat.size, 4);
  ASSERT_EQ(f12_rename(&fs, "OLD.BIN", "NEW.BIN"), F12_OK);
  ASSERT_EQ(f12_stat(&fs, "OLD.BIN", &stat), F12_ERR_NOT_FOUND);
  ASSERT_EQ(f12_stat(&fs, "NEW.BIN", &stat), F12_OK);
  ASSERT_EQ(f12_delete(&fs, "NEW.BIN"), F12_OK);
  ASSERT_EQ(f12_stat(&fs, "NEW.BIN", &stat), F12_ERR_NOT_FOUND);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_free_count_is_typed_and_transaction_safe) {
  prepare();
  uint16_t before;
  ASSERT_EQ(f12_free_count(&fs, &before), F12_OK);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "COUNT.BIN", F12_OPEN_WRITE, &file), F12_OK);
  uint16_t count = UINT16_MAX;
  ASSERT_EQ(f12_free_count(&fs, &count), F12_ERR_CONFLICT);
  ASSERT_EQ(count, 0);
  ASSERT_EQ(f12_abort(&file), F12_OK);
  ASSERT_EQ(f12_free_count(&fs, &count), F12_OK);
  ASSERT_EQ(count, before);
  ASSERT_EQ(f12_free_count(&fs, NULL), F12_ERR_INVALID);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_write_protection) {
  prepare();
  disk.write_protected = true;
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "PROTECT.BIN", F12_OPEN_WRITE, &file),
            F12_ERR_WRITE_PROTECTED);
  ASSERT_EQ(f12_delete(&fs, "NONE.BIN"), F12_ERR_WRITE_PROTECTED);
  disk.write_protected = false;
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_generation_observer_errors_are_exact) {
  disk_reset();
  ASSERT_EQ(f12_init(&fs, disk_device(true)), F12_OK);
  disk.generation_status = BLOCK_ERR_TIMEOUT;
  ASSERT_EQ(f12_mount(&fs), F12_ERR_TIMEOUT);
  disk.generation_status = BLOCK_OK;
  ASSERT_EQ(f12_format(&fs, format_options("OBSERVER", F12_FORMAT_QUICK)),
            F12_OK);
  ASSERT_EQ(f12_mount(&fs), F12_OK);
  disk.generation_status = BLOCK_ERR_BUSY;
  bool mounted = true;
  ASSERT_EQ(f12_is_mounted(&fs, &mounted), F12_ERR_BUSY);
  ASSERT(!mounted);
  f12_stat_t stat;
  ASSERT_EQ(f12_stat(&fs, "NONE.BIN", &stat), F12_ERR_BUSY);
  ASSERT_EQ(stat.name[0], '\0');
  disk.generation_status = BLOCK_OK;
  ASSERT(filesystem_mounted());
  disk.generation_status = BLOCK_ERR_MEDIA_CHANGED;
  mounted = true;
  ASSERT_EQ(f12_is_mounted(&fs, &mounted), F12_ERR_MEDIA_CHANGED);
  ASSERT(!mounted);
  disk.generation_status = BLOCK_OK;
  ASSERT(!filesystem_mounted());
}

TEST(test_write_protection_observer_errors_are_exact) {
  disk_reset();
  ASSERT_EQ(f12_init(&fs, disk_device(true)), F12_OK);
  disk.write_protected_status = BLOCK_ERR_IO;
  ASSERT_EQ(f12_format(&fs, format_options("OBSERVER", F12_FORMAT_QUICK)),
            F12_ERR_IO);
  disk.write_protected_status = BLOCK_OK;
  ASSERT_EQ(f12_format(&fs, format_options("OBSERVER", F12_FORMAT_QUICK)),
            F12_OK);
  ASSERT_EQ(f12_mount(&fs), F12_OK);
  disk.write_protected_status = BLOCK_ERR_TIMEOUT;
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "FAIL.BIN", F12_OPEN_WRITE, &file), F12_ERR_TIMEOUT);
  disk.write_protected_status = BLOCK_OK;
  ASSERT(filesystem_mounted());
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_abort_discards_uncommitted_writer) {
  prepare();
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "ABORT.BIN", F12_OPEN_WRITE, &file), F12_OK);
  ASSERT_EQ(f12_write(&file, "discard", 7).error, F12_OK);
  ASSERT_EQ(f12_abort(&file), F12_OK);
  f12_stat_t stat;
  ASSERT_EQ(f12_stat(&fs, "ABORT.BIN", &stat), F12_ERR_NOT_FOUND);
  ASSERT_EQ(f12_close(&file), F12_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_failed_close_is_retryable_and_not_abortable) {
  prepare();
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "RETRY.BIN", F12_OPEN_WRITE, &file), F12_OK);
  ASSERT_EQ(f12_write(&file, "retry", 5).error, F12_OK);
  disk.write_status = BLOCK_ERR_VERIFY;
  ASSERT_EQ(f12_close(&file), F12_ERR_VERIFY);
  ASSERT_EQ(f12_abort(&file), F12_ERR_BUSY);
  disk.write_status = BLOCK_OK;
  ASSERT_EQ(f12_close(&file), F12_OK);
  char data[5];
  ASSERT_EQ(read_file("RETRY.BIN", data, sizeof(data)), sizeof(data));
  ASSERT_MEM_EQ(data, "retry", sizeof(data));
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_unmount_preserves_open_handle_ownership) {
  prepare();
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "UMOUNT.BIN", F12_OPEN_WRITE, &file), F12_OK);
  ASSERT_EQ(f12_write(&file, "x", 1).error, F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_ERR_CONFLICT);
  disk.write_status = BLOCK_ERR_TIMEOUT;
  ASSERT_EQ(f12_close(&file), F12_ERR_TIMEOUT);
  ASSERT_EQ(f12_unmount(&fs), F12_ERR_CONFLICT);
  ASSERT(filesystem_mounted());
  disk.write_status = BLOCK_OK;
  ASSERT_EQ(f12_close(&file), F12_OK);
  f12_file_t reader;
  ASSERT_EQ(f12_open(&fs, "UMOUNT.BIN", F12_OPEN_READ, &reader), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_ERR_CONFLICT);
  char value;
  ASSERT_EQ(f12_read(&reader, &value, 1).error, F12_OK);
  ASSERT_EQ(value, 'x');
  ASSERT_EQ(f12_close(&reader), F12_OK);
  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_ERR_CONFLICT);
  f12_stat_t stat;
  ASSERT_EQ(f12_readdir(&dir, &stat), F12_OK);
  ASSERT_EQ(f12_closedir(&dir), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_fsck_conflict_and_clean) {
  prepare();
  fat12_fsck_t report;
  ASSERT_EQ(f12_fsck(&fs, &report, false), F12_OK);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "FSCK.BIN", F12_OPEN_WRITE, &file), F12_OK);
  ASSERT_EQ(f12_fsck(&fs, &report, false), F12_ERR_CONFLICT);
  ASSERT_EQ(f12_fsck(&fs, &report, true), F12_ERR_CONFLICT);
  ASSERT_EQ(f12_abort(&file), F12_OK);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_mount_block_errors_are_exact) {
  static const struct {
    block_status_t block;
    f12_err_t f12;
  } cases[] = {
      {BLOCK_ERR_TIMEOUT, F12_ERR_TIMEOUT},
      {BLOCK_ERR_BUSY, F12_ERR_BUSY},
      {BLOCK_ERR_CRC, F12_ERR_CRC},
      {BLOCK_ERR_WRONG_TRACK, F12_ERR_WRONG_TRACK},
      {BLOCK_ERR_WRONG_SIDE, F12_ERR_WRONG_SIDE},
      {BLOCK_ERR_NO_TRACK0, F12_ERR_NO_TRACK0},
      {BLOCK_ERR_MEDIA_CHANGED, F12_ERR_MEDIA_CHANGED},
      {BLOCK_ERR_WRITE_PROTECTED, F12_ERR_WRITE_PROTECTED},
      {BLOCK_ERR_UNDERRUN, F12_ERR_UNDERRUN},
      {BLOCK_ERR_OVERRUN, F12_ERR_OVERRUN},
      {BLOCK_ERR_VERIFY, F12_ERR_VERIFY},
      {BLOCK_ERR_CORRUPT, F12_ERR_CORRUPT},
      {BLOCK_ERR_IO, F12_ERR_IO},
  };

  disk_reset();
  ASSERT_EQ(f12_init(&fs, disk_device(true)), F12_OK);
  ASSERT_EQ(f12_format(&fs, format_options("ERRORS", F12_FORMAT_QUICK)), F12_OK);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    ASSERT_EQ(f12_init(&fs, disk_device(true)), F12_OK);
    disk.read_status = cases[i].block;
    ASSERT_EQ(f12_mount(&fs), cases[i].f12);
    disk.read_status = BLOCK_OK;
  }
}

TEST(test_format_block_error_is_exact) {
  disk_reset();
  ASSERT_EQ(f12_init(&fs, disk_device(true)), F12_OK);
  disk.write_status = BLOCK_ERR_VERIFY;
  ASSERT_EQ(f12_format(&fs, format_options("FAIL", F12_FORMAT_FULL)),
            F12_ERR_VERIFY);
}

TEST(test_internal_generations_and_cache_clock_skip_zero_on_wrap) {
  prepare();

  fs.mount_generation = UINT64_MAX;
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
  ASSERT_EQ(fs.mount_generation, 1);
  ASSERT_EQ(f12_mount(&fs), F12_OK);
  ASSERT_EQ(fs.mount_generation, 2);

  fs.files[0].generation = UINT64_MAX;
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "WRAP.BIN", F12_OPEN_WRITE, &file), F12_OK);
  ASSERT_EQ(file.token.slot, 0);
  ASSERT_EQ(file.token.slot_generation, 1);
  ASSERT_EQ(f12_abort(&file), F12_OK);

  fs.dirs[0].generation = UINT64_MAX;
  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), F12_OK);
  ASSERT_EQ(dir.token.slot, 0);
  ASSERT_EQ(dir.token.slot_generation, 1);
  ASSERT_EQ(f12_closedir(&dir), F12_OK);

  memset(fs.cache, 0, sizeof(fs.cache));
  fs.cache_clock = 0;
  uint8_t sector[DISK_SECTOR_SIZE];
  for (uint16_t track = 0; track < F12_CACHE_TRACKS; track++) {
    ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx,
                             track * DISK_SECTORS_PER_TRACK, sector), BLOCK_OK);
  }
  fs.cache_clock = UINT64_MAX;
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx, 0, sector), BLOCK_OK);
  ASSERT_EQ(fs.cache_clock, 1);
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx,
                           F12_CACHE_TRACKS * DISK_SECTORS_PER_TRACK,
                           sector), BLOCK_OK);
  ASSERT_EQ(fs.fat.io.read(fs.fat.io.ctx,
                           (F12_CACHE_TRACKS + 1u) * DISK_SECTORS_PER_TRACK,
                           sector), BLOCK_OK);
  ASSERT(cache_has(0, 0));
  ASSERT_EQ(fs.cache_clock, 3);
  ASSERT_EQ(f12_unmount(&fs), F12_OK);
}

TEST(test_strerror_is_total) {
  for (int error = F12_OK; error <= F12_ERR_IO; error++) {
    ASSERT(strcmp(f12_strerror((f12_err_t)error), "unknown error") != 0);
  }
}

int main(void) {
  printf("=== F12 Contract Tests ===\n\n");
  RUN_TEST(test_init_contract);
  RUN_TEST(test_format_mount_unmount);
  RUN_TEST(test_format_options_and_progress_context);
  RUN_TEST(test_read_only_device_mounts);
  RUN_TEST(test_block_adapter_rejects_null_context_and_output);
  RUN_TEST(test_track_cache_is_atomic_lru);
  RUN_TEST(test_partial_crc_track_serves_valid_sector);
  RUN_TEST(test_corrupt_full_track_rejected);
  RUN_TEST(test_partial_write_materializes_full_track);
  RUN_TEST(test_failed_write_does_not_poison_cache);
  RUN_TEST(test_failed_mutating_write_evicts_cache);
  RUN_TEST(test_conflicting_partial_reads_never_form_a_track);
  RUN_TEST(test_file_roundtrip_and_end);
  RUN_TEST(test_seek_tell_and_read_at);
  RUN_TEST(test_mode_errors_are_typed);
  RUN_TEST(test_stale_file_handle_generation);
  RUN_TEST(test_stale_mount_generation);
  RUN_TEST(test_media_generation_invalidates_mount);
  RUN_TEST(test_tell_observes_media_generation);
  RUN_TEST(test_cache_hit_generation_race_is_detected);
  RUN_TEST(test_reinit_does_not_resurrect_handles);
  RUN_TEST(test_live_reinit_resets_and_invalidates_handles);
  RUN_TEST(test_invalid_reinit_preserves_live_context);
  RUN_TEST(test_media_change_forgets_commit_phase_writer);
  RUN_TEST(test_open_conflicts);
  RUN_TEST(test_multiple_readers_and_limit);
  RUN_TEST(test_directory_end_and_generation);
  RUN_TEST(test_readdir_preserves_io_error);
  RUN_TEST(test_list_propagates_callback_error);
  RUN_TEST(test_delete_rename_and_stat);
  RUN_TEST(test_free_count_is_typed_and_transaction_safe);
  RUN_TEST(test_write_protection);
  RUN_TEST(test_generation_observer_errors_are_exact);
  RUN_TEST(test_write_protection_observer_errors_are_exact);
  RUN_TEST(test_abort_discards_uncommitted_writer);
  RUN_TEST(test_failed_close_is_retryable_and_not_abortable);
  RUN_TEST(test_unmount_preserves_open_handle_ownership);
  RUN_TEST(test_fsck_conflict_and_clean);
  RUN_TEST(test_mount_block_errors_are_exact);
  RUN_TEST(test_format_block_error_is_exact);
  RUN_TEST(test_internal_generations_and_cache_clock_skip_zero_on_wrap);
  RUN_TEST(test_strerror_is_total);
  TEST_RESULTS();
}
