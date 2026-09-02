#include "test.h"
#include "vdisk.h"
#include "../src/f12.h"

typedef struct {
  void *expected;
  uint32_t calls;
  uint32_t generation_calls_at_first_track;
} progress_state_t;

static vdisk_t disk;
static f12_t fs;

static bool filesystem_mounted(void) {
  bool mounted = false;
  ASSERT_EQ(f12_is_mounted(&fs, &mounted), DISK_OK);
  return mounted;
}

static void progress(void *ctx, uint8_t cylinder, uint8_t head,
                     uint16_t done, uint16_t total) {
  progress_state_t *state = (progress_state_t *)ctx;
  ASSERT(state == state->expected);
  ASSERT(cylinder < DISK_CYLINDERS);
  ASSERT(head < DISK_HEADS);
  ASSERT(done <= total);
  state->calls++;
  if (done == 1) state->generation_calls_at_first_track = disk.generation_calls;
}

static f12_format_options_t format_options(const char *label,
                                           f12_format_mode_t mode) {
  return (f12_format_options_t){.label = label, .mode = mode};
}

static void prepare(void) {
  vdisk_init(&disk);
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  ASSERT_EQ(f12_format(&fs, format_options("TEST", F12_FORMAT_QUICK)), DISK_OK);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
}

static void write_file(const char *name, const void *data, size_t size) {
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, name, F12_OPEN_WRITE, &file), DISK_OK);
  if (size != 0) {
    disk_result_t result = f12_write(&file, data, size);
    ASSERT_EQ(result.error, DISK_OK);
    ASSERT_EQ(result.count, size);
  }
  ASSERT_EQ(f12_close(&file), DISK_OK);
}

static size_t read_file(const char *name, void *data, size_t capacity) {
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, name, F12_OPEN_READ, &file), DISK_OK);
  disk_result_t result = f12_read(&file, data, capacity);
  ASSERT(result.error == DISK_OK || result.error == DISK_END);
  ASSERT_EQ(f12_close(&file), DISK_OK);
  return result.count;
}

TEST(test_init_contract) {
  vdisk_init(&disk);
  f12_t automatic;
  ASSERT_EQ(f12_init(&automatic, vdisk_device(&disk)), DISK_OK);
  bool mounted = true;
  ASSERT_EQ(f12_is_mounted(&automatic, &mounted), DISK_OK);
  ASSERT(!mounted);
  disk_device_t device = vdisk_device(&disk);
  ASSERT_EQ(f12_init(NULL, device), DISK_ERR_INVALID);
  device.read_track = NULL;
  ASSERT_EQ(f12_init(&fs, device), DISK_ERR_INVALID);
  device = vdisk_device(&disk);
  device.media_generation = NULL;
  ASSERT_EQ(f12_init(&fs, device), DISK_ERR_INVALID);
  device = vdisk_device(&disk);
  device.write_protected = NULL;
  ASSERT_EQ(f12_init(&fs, device), DISK_ERR_INVALID);
  ASSERT_EQ(f12_init(&fs, vdisk_readonly_device(&disk)), DISK_OK);
}

TEST(test_format_mount_unmount) {
  prepare();
  ASSERT(filesystem_mounted());
  ASSERT_EQ(f12_mount(&fs), DISK_ERR_ALREADY_MOUNTED);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
  ASSERT(!filesystem_mounted());
  ASSERT_EQ(f12_unmount(&fs), DISK_ERR_NOT_MOUNTED);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_format_options_and_progress_context) {
  vdisk_init(&disk);
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  progress_state_t state = {.expected = &state};
  f12_format_options_t options = {
      .label = "FULL",
      .mode = F12_FORMAT_FULL,
      .progress = progress,
      .progress_ctx = &state,
  };
  ASSERT_EQ(f12_format(&fs, options), DISK_OK);
  ASSERT_EQ(state.calls, DISK_TRACK_COUNT);
  ASSERT_EQ(disk.track_writes, DISK_TRACK_COUNT);
  options.mode = (f12_format_mode_t)99;
  ASSERT_EQ(f12_format(&fs, options), DISK_ERR_INVALID);
  options.mode = F12_FORMAT_QUICK;
  options.label = NULL;
  ASSERT_EQ(f12_format(&fs, options), DISK_ERR_INVALID);
}

TEST(test_quick_format_needs_no_reads_from_blank_media) {
  vdisk_init(&disk);
  disk.read_status = DISK_ERR_TIMEOUT;
  disk.error_valid = 0;
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  ASSERT_EQ(f12_format(&fs, format_options("BLANK", F12_FORMAT_QUICK)), DISK_OK);
  ASSERT_EQ(disk.track_writes, 2);
  disk.read_status = DISK_OK;
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_mount_adopts_media_changed_since_the_last_look) {
  prepare();
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
  disk.generation_calls = 0;
  disk.change_generation_on_call = 2;
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  ASSERT(filesystem_mounted());
  uint16_t count = 0;
  ASSERT_EQ(f12_free_count(&fs, &count), DISK_OK);
  ASSERT_EQ(count, FAT12_DATA_CLUSTERS);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_format_adopts_media_changed_only_before_the_first_track) {
  vdisk_init(&disk);
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  disk.change_generation_on_call = 2;
  ASSERT_EQ(f12_format(&fs, format_options("SWAPPED", F12_FORMAT_QUICK)), DISK_OK);
  ASSERT_EQ(disk.track_writes, 2);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);

  vdisk_init(&disk);
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  progress_state_t state = {.expected = &state};
  f12_format_options_t options = {
      .label = "MIDWAY",
      .mode = F12_FORMAT_QUICK,
      .progress = progress,
      .progress_ctx = &state,
  };
  ASSERT_EQ(f12_format(&fs, options), DISK_OK);
  ASSERT_EQ(state.calls, 2);
  vdisk_init(&disk);
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  state.calls = 0;
  disk.change_generation_on_call = state.generation_calls_at_first_track + 1u;
  ASSERT_EQ(f12_format(&fs, options), DISK_ERR_MEDIA_CHANGED);
  ASSERT_EQ(disk.track_writes, 1);
  ASSERT_EQ(state.calls, 1);
}

TEST(test_read_only_device_mounts) {
  prepare();
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
  ASSERT_EQ(f12_init(&fs, vdisk_readonly_device(&disk)), DISK_OK);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "NEW.TXT", F12_OPEN_WRITE, &file),
            DISK_ERR_WRITE_PROTECTED);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_corrupt_full_track_rejected) {
  vdisk_init(&disk);
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  disk.corrupt_track = true;
  ASSERT_EQ(f12_mount(&fs), DISK_ERR_CORRUPT);
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
  ASSERT_EQ(f12_open(&fs, "ROUND.BIN", F12_OPEN_READ, &file), DISK_OK);
  disk_result_t result = f12_read(&file, target, sizeof(target));
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, sizeof(target));
  result = f12_read(&file, target, 1);
  ASSERT_EQ(result.error, DISK_END);
  ASSERT_EQ(result.count, 0);
  ASSERT_EQ(f12_close(&file), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_seek_tell_and_read_at) {
  prepare();
  uint8_t source[1024];
  for (size_t i = 0; i < sizeof(source); i++) source[i] = (uint8_t)i;
  write_file("SEEK.BIN", source, sizeof(source));
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "SEEK.BIN", F12_OPEN_READ, &file), DISK_OK);
  ASSERT_EQ(f12_seek(&file, 700), DISK_OK);
  uint32_t offset;
  ASSERT_EQ(f12_tell(&file, &offset), DISK_OK);
  ASSERT_EQ(offset, 700);
  uint8_t data[20];
  disk_result_t result = f12_read_at(&file, 100, data, sizeof(data));
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, sizeof(data));
  ASSERT_MEM_EQ(data, source + 100, sizeof(data));
  ASSERT_EQ(f12_tell(&file, &offset), DISK_OK);
  ASSERT_EQ(offset, 700);
  cache_clear(&fs.cache);
  disk.read_status = DISK_ERR_TIMEOUT;
  result = f12_read_at(&file, 600, data, sizeof(data));
  ASSERT_EQ(result.error, DISK_ERR_TIMEOUT);
  ASSERT_EQ(result.count, 0);
  ASSERT_EQ(f12_tell(&file, &offset), DISK_OK);
  ASSERT_EQ(offset, 700);
  disk.read_status = DISK_OK;
  ASSERT_EQ(f12_close(&file), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_mode_errors_are_typed) {
  prepare();
  write_file("MODE.BIN", "x", 1);
  f12_file_t reader;
  ASSERT_EQ(f12_open(&fs, "MODE.BIN", F12_OPEN_READ, &reader), DISK_OK);
  ASSERT_EQ(f12_write(&reader, "x", 1).error, DISK_ERR_CONFLICT);
  ASSERT_EQ(f12_close(&reader), DISK_OK);
  f12_file_t writer;
  ASSERT_EQ(f12_open(&fs, "MODE.BIN", F12_OPEN_WRITE, &writer), DISK_OK);
  uint8_t byte;
  ASSERT_EQ(f12_read(&writer, &byte, 1).error, DISK_ERR_CONFLICT);
  ASSERT_EQ(f12_seek(&writer, 0), DISK_ERR_CONFLICT);
  ASSERT_EQ(f12_abort(&writer), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_stale_file_handle_generation) {
  prepare();
  write_file("ONE.BIN", "1", 1);
  write_file("TWO.BIN", "2", 1);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "ONE.BIN", F12_OPEN_READ, &file), DISK_OK);
  f12_file_t stale = file;
  ASSERT_EQ(f12_close(&file), DISK_OK);
  ASSERT_EQ(f12_open(&fs, "TWO.BIN", F12_OPEN_READ, &file), DISK_OK);
  uint8_t byte;
  ASSERT_EQ(f12_read(&stale, &byte, 1).error, DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_close(&stale), DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_close(&file), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_handles_do_not_survive_remount) {
  prepare();
  write_file("STALE.BIN", "x", 1);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "STALE.BIN", F12_OPEN_READ, &file), DISK_OK);
  f12_file_t stale = file;
  ASSERT_EQ(f12_unmount(&fs), DISK_ERR_CONFLICT);
  ASSERT_EQ(f12_close(&file), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  uint8_t byte;
  ASSERT_EQ(f12_read(&stale, &byte, 1).error, DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_close(&stale), DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_open(&fs, "STALE.BIN", F12_OPEN_READ, &file), DISK_OK);
  ASSERT_EQ(f12_read(&stale, &byte, 1).error, DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_close(&file), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_media_generation_invalidates_mount) {
  prepare();
  write_file("MEDIA.BIN", "x", 1);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "MEDIA.BIN", F12_OPEN_READ, &file), DISK_OK);
  disk.generation++;
  uint8_t byte;
  ASSERT_EQ(f12_read(&file, &byte, 1).error, DISK_ERR_MEDIA_CHANGED);
  ASSERT(!filesystem_mounted());
  ASSERT_EQ(f12_close(&file), DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_tell_observes_media_generation) {
  prepare();
  write_file("TELL.BIN", "x", 1);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "TELL.BIN", F12_OPEN_READ, &file), DISK_OK);
  disk.generation++;
  uint32_t offset;
  ASSERT_EQ(f12_tell(&file, &offset), DISK_ERR_MEDIA_CHANGED);
  ASSERT(!filesystem_mounted());
  ASSERT_EQ(f12_close(&file), DISK_ERR_BAD_HANDLE);
}

TEST(test_reinit_does_not_resurrect_handles) {
  prepare();
  write_file("ABA.BIN", "a", 1);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "ABA.BIN", F12_OPEN_READ, &file), DISK_OK);
  f12_file_t stale = file;
  ASSERT_EQ(f12_close(&file), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  ASSERT_EQ(f12_open(&fs, "ABA.BIN", F12_OPEN_READ, &file), DISK_OK);
  uint8_t value;
  ASSERT_EQ(f12_read(&stale, &value, 1).error, DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_close(&file), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_live_reinit_resets_and_invalidates_handles) {
  prepare();
  write_file("LIVE.BIN", "x", 1);
  f12_file_t file;
  f12_dir_t dir;
  ASSERT_EQ(f12_open(&fs, "LIVE.BIN", F12_OPEN_READ, &file), DISK_OK);
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), DISK_OK);
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  ASSERT(!filesystem_mounted());
  uint8_t byte;
  f12_stat_t stat;
  ASSERT_EQ(f12_read(&file, &byte, 1).error, DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_readdir(&dir, &stat), DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_invalid_reinit_preserves_live_context) {
  prepare();
  write_file("PRESERVE.BIN", "p", 1);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "PRESERVE.BIN", F12_OPEN_READ, &file), DISK_OK);
  disk_device_t invalid = vdisk_device(&disk);
  invalid.media_generation = NULL;
  ASSERT_EQ(f12_init(&fs, invalid), DISK_ERR_INVALID);
  ASSERT(filesystem_mounted());
  uint8_t value = 0;
  ASSERT_EQ(f12_read(&file, &value, 1).error, DISK_OK);
  ASSERT_EQ(value, 'p');
  ASSERT_EQ(f12_close(&file), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_media_change_forgets_commit_phase_writer) {
  prepare();
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "SWAP.BIN", F12_OPEN_WRITE, &file), DISK_OK);
  ASSERT_EQ(f12_write(&file, "swap", 4).error, DISK_OK);
  disk.write_status = DISK_ERR_VERIFY;
  ASSERT_EQ(f12_close(&file), DISK_ERR_VERIFY);
  disk.write_status = DISK_OK;
  disk.generation++;
  ASSERT_EQ(f12_close(&file), DISK_ERR_MEDIA_CHANGED);
  ASSERT_EQ(f12_close(&file), DISK_ERR_BAD_HANDLE);
  ASSERT(!cache_dirty(&fs.cache));
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_open_conflicts) {
  prepare();
  write_file("A.BIN", "a", 1);
  write_file("B.BIN", "b", 1);
  f12_file_t reader;
  f12_file_t writer;
  f12_file_t other;
  ASSERT_EQ(f12_open(&fs, "A.BIN", F12_OPEN_READ, &reader), DISK_OK);
  ASSERT_EQ(f12_open(&fs, "A.BIN", F12_OPEN_WRITE, &writer), DISK_ERR_CONFLICT);
  ASSERT_EQ(f12_open(&fs, "B.BIN", F12_OPEN_WRITE, &writer), DISK_OK);
  ASSERT_EQ(f12_open(&fs, "B.BIN", F12_OPEN_READ, &other), DISK_ERR_CONFLICT);
  ASSERT_EQ(f12_open(&fs, "C.BIN", F12_OPEN_WRITE, &other), DISK_ERR_CONFLICT);
  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), DISK_ERR_CONFLICT);
  ASSERT_EQ(f12_delete(&fs, "A.BIN"), DISK_ERR_CONFLICT);
  ASSERT_EQ(f12_rename(&fs, "A.BIN", "C.BIN"), DISK_ERR_CONFLICT);
  ASSERT_EQ(f12_close(&reader), DISK_OK);
  ASSERT_EQ(f12_close(&writer), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_multiple_readers_and_limit) {
  prepare();
  write_file("READ.BIN", "x", 1);
  f12_file_t files[F12_MAX_OPEN_FILES];
  for (size_t i = 0; i < F12_MAX_OPEN_FILES; i++) {
    ASSERT_EQ(f12_open(&fs, "READ.BIN", F12_OPEN_READ, &files[i]), DISK_OK);
  }
  f12_file_t extra;
  ASSERT_EQ(f12_open(&fs, "READ.BIN", F12_OPEN_READ, &extra), DISK_ERR_TOO_MANY);
  for (size_t i = 0; i < F12_MAX_OPEN_FILES; i++) {
    ASSERT_EQ(f12_close(&files[i]), DISK_OK);
  }
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_directory_end_and_generation) {
  prepare();
  write_file("DIR.BIN", "x", 1);
  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), DISK_OK);
  f12_dir_t stale = dir;
  f12_stat_t stat;
  ASSERT_EQ(f12_readdir(&dir, &stat), DISK_OK);
  ASSERT_STR_EQ(stat.name, "DIR.BIN");
  ASSERT_EQ(f12_readdir(&dir, &stat), DISK_END);
  ASSERT_EQ(f12_closedir(&dir), DISK_OK);
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), DISK_OK);
  ASSERT_EQ(f12_readdir(&stale, &stat), DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_closedir(&stale), DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_closedir(&dir), DISK_OK);
  ASSERT_EQ(f12_opendir(&fs, "/SUB", &dir), DISK_ERR_NOT_DIR);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_readdir_preserves_io_error) {
  prepare();
  write_file("IO.BIN", "x", 1);
  cache_clear(&fs.cache);
  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), DISK_OK);
  disk.read_status = DISK_ERR_TIMEOUT;
  f12_stat_t stat;
  ASSERT_EQ(f12_readdir(&dir, &stat), DISK_ERR_TIMEOUT);
  disk.read_status = DISK_OK;
  ASSERT_EQ(f12_closedir(&dir), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

static disk_err_t list_fail(void *ctx, const f12_stat_t *stat) {
  size_t *calls = (size_t *)ctx;
  ASSERT(stat != NULL);
  (*calls)++;
  return DISK_ERR_CONFLICT;
}

TEST(test_list_propagates_callback_error) {
  prepare();
  write_file("LIST.BIN", "x", 1);
  size_t calls = 0;
  ASSERT_EQ(f12_list(&fs, list_fail, &calls), DISK_ERR_CONFLICT);
  ASSERT_EQ(calls, 1);
  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), DISK_OK);
  ASSERT_EQ(f12_closedir(&dir), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

static disk_err_t list_count(void *ctx, const f12_stat_t *stat) {
  size_t *count = (size_t *)ctx;
  ASSERT(stat->name[0] != '\0');
  (*count)++;
  return DISK_OK;
}

TEST(test_list_visits_every_entry_and_releases_its_directory) {
  prepare();
  write_file("ONE.BIN", "1", 1);
  write_file("TWO.BIN", "22", 2);
  size_t count = 0;
  ASSERT_EQ(f12_list(&fs, list_count, &count), DISK_OK);
  ASSERT_EQ(count, 2);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_tell_follows_writer_progress) {
  prepare();
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "TELLW.BIN", F12_OPEN_WRITE, &file), DISK_OK);
  uint32_t offset = UINT32_MAX;
  ASSERT_EQ(f12_tell(&file, &offset), DISK_OK);
  ASSERT_EQ(offset, 0);
  ASSERT_EQ(f12_write(&file, "progress", 8).error, DISK_OK);
  ASSERT_EQ(f12_tell(&file, &offset), DISK_OK);
  ASSERT_EQ(offset, 8);
  ASSERT_EQ(f12_write(&file, "!", 1).error, DISK_OK);
  ASSERT_EQ(f12_tell(&file, &offset), DISK_OK);
  ASSERT_EQ(offset, 9);
  ASSERT_EQ(f12_close(&file), DISK_OK);
  f12_stat_t stat;
  ASSERT_EQ(f12_stat(&fs, "TELLW.BIN", &stat), DISK_OK);
  ASSERT_EQ(stat.size, 9);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_read_at_zero_length_leaves_position_untouched) {
  prepare();
  uint8_t source[700];
  for (size_t i = 0; i < sizeof(source); i++) source[i] = (uint8_t)(i * 3u);
  write_file("AT.BIN", source, sizeof(source));
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "AT.BIN", F12_OPEN_READ, &file), DISK_OK);
  ASSERT_EQ(f12_seek(&file, 100), DISK_OK);
  uint8_t data[8];
  disk_result_t result = f12_read_at(&file, 600, data, 0);
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, 0);
  uint32_t offset;
  ASSERT_EQ(f12_tell(&file, &offset), DISK_OK);
  ASSERT_EQ(offset, 100);
  result = f12_read(&file, data, sizeof(data));
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, sizeof(data));
  ASSERT_MEM_EQ(data, source + 100, sizeof(data));
  ASSERT_EQ(f12_close(&file), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_delete_rename_and_stat) {
  prepare();
  write_file("OLD.BIN", "data", 4);
  f12_stat_t stat;
  ASSERT_EQ(f12_stat(&fs, "OLD.BIN", &stat), DISK_OK);
  ASSERT_EQ(stat.size, 4);
  ASSERT_EQ(f12_rename(&fs, "OLD.BIN", "NEW.BIN"), DISK_OK);
  ASSERT_EQ(f12_stat(&fs, "OLD.BIN", &stat), DISK_ERR_NOT_FOUND);
  ASSERT_EQ(f12_stat(&fs, "NEW.BIN", &stat), DISK_OK);
  ASSERT_EQ(f12_delete(&fs, "NEW.BIN"), DISK_OK);
  ASSERT_EQ(f12_stat(&fs, "NEW.BIN", &stat), DISK_ERR_NOT_FOUND);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_free_count_is_typed_and_transaction_safe) {
  prepare();
  uint16_t before;
  ASSERT_EQ(f12_free_count(&fs, &before), DISK_OK);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "COUNT.BIN", F12_OPEN_WRITE, &file), DISK_OK);
  uint16_t count = UINT16_MAX;
  ASSERT_EQ(f12_free_count(&fs, &count), DISK_ERR_CONFLICT);
  ASSERT_EQ(count, 0);
  ASSERT_EQ(f12_abort(&file), DISK_OK);
  ASSERT_EQ(f12_free_count(&fs, &count), DISK_OK);
  ASSERT_EQ(count, before);
  ASSERT_EQ(f12_free_count(&fs, NULL), DISK_ERR_INVALID);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_write_protection) {
  prepare();
  disk.write_protected = true;
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "PROTECT.BIN", F12_OPEN_WRITE, &file),
            DISK_ERR_WRITE_PROTECTED);
  ASSERT_EQ(f12_delete(&fs, "NONE.BIN"), DISK_ERR_WRITE_PROTECTED);
  disk.write_protected = false;
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_generation_observer_errors_are_exact) {
  vdisk_init(&disk);
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  disk.generation_status = DISK_ERR_TIMEOUT;
  ASSERT_EQ(f12_mount(&fs), DISK_ERR_TIMEOUT);
  disk.generation_status = DISK_OK;
  ASSERT_EQ(f12_format(&fs, format_options("OBSERVER", F12_FORMAT_QUICK)),
            DISK_OK);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  disk.generation_status = DISK_ERR_BUSY;
  bool mounted = true;
  ASSERT_EQ(f12_is_mounted(&fs, &mounted), DISK_ERR_BUSY);
  ASSERT(!mounted);
  f12_stat_t stat;
  ASSERT_EQ(f12_stat(&fs, "NONE.BIN", &stat), DISK_ERR_BUSY);
  ASSERT_EQ(stat.name[0], '\0');
  disk.generation_status = DISK_OK;
  ASSERT(filesystem_mounted());
  disk.generation_status = DISK_ERR_MEDIA_CHANGED;
  mounted = true;
  ASSERT_EQ(f12_is_mounted(&fs, &mounted), DISK_ERR_MEDIA_CHANGED);
  ASSERT(!mounted);
  disk.generation_status = DISK_OK;
  ASSERT(!filesystem_mounted());
}

TEST(test_write_protection_observer_errors_are_exact) {
  vdisk_init(&disk);
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  disk.write_protected_status = DISK_ERR_IO;
  ASSERT_EQ(f12_format(&fs, format_options("OBSERVER", F12_FORMAT_QUICK)),
            DISK_ERR_IO);
  disk.write_protected_status = DISK_OK;
  ASSERT_EQ(f12_format(&fs, format_options("OBSERVER", F12_FORMAT_QUICK)),
            DISK_OK);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  disk.write_protected_status = DISK_ERR_TIMEOUT;
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "FAIL.BIN", F12_OPEN_WRITE, &file), DISK_ERR_TIMEOUT);
  disk.write_protected_status = DISK_OK;
  ASSERT(filesystem_mounted());
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_abort_discards_uncommitted_writer) {
  prepare();
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "ABORT.BIN", F12_OPEN_WRITE, &file), DISK_OK);
  ASSERT_EQ(f12_write(&file, "discard", 7).error, DISK_OK);
  ASSERT(cache_dirty(&fs.cache));
  ASSERT_EQ(f12_abort(&file), DISK_OK);
  ASSERT(!cache_dirty(&fs.cache));
  f12_stat_t stat;
  ASSERT_EQ(f12_stat(&fs, "ABORT.BIN", &stat), DISK_ERR_NOT_FOUND);
  ASSERT_EQ(f12_close(&file), DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_failed_close_is_retryable_and_not_abortable) {
  prepare();
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "RETRY.BIN", F12_OPEN_WRITE, &file), DISK_OK);
  ASSERT_EQ(f12_write(&file, "retry", 5).error, DISK_OK);
  disk.write_status = DISK_ERR_VERIFY;
  ASSERT_EQ(f12_close(&file), DISK_ERR_VERIFY);
  ASSERT_EQ(f12_abort(&file), DISK_ERR_BUSY);
  disk.write_status = DISK_OK;
  ASSERT_EQ(f12_close(&file), DISK_OK);
  char data[5];
  ASSERT_EQ(read_file("RETRY.BIN", data, sizeof(data)), sizeof(data));
  ASSERT_MEM_EQ(data, "retry", sizeof(data));
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_unmount_preserves_open_handle_ownership) {
  prepare();
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "UMOUNT.BIN", F12_OPEN_WRITE, &file), DISK_OK);
  ASSERT_EQ(f12_write(&file, "x", 1).error, DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_ERR_CONFLICT);
  disk.write_status = DISK_ERR_TIMEOUT;
  ASSERT_EQ(f12_close(&file), DISK_ERR_TIMEOUT);
  ASSERT_EQ(f12_unmount(&fs), DISK_ERR_CONFLICT);
  ASSERT(filesystem_mounted());
  disk.write_status = DISK_OK;
  ASSERT_EQ(f12_close(&file), DISK_OK);
  f12_file_t reader;
  ASSERT_EQ(f12_open(&fs, "UMOUNT.BIN", F12_OPEN_READ, &reader), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_ERR_CONFLICT);
  char value;
  ASSERT_EQ(f12_read(&reader, &value, 1).error, DISK_OK);
  ASSERT_EQ(value, 'x');
  ASSERT_EQ(f12_close(&reader), DISK_OK);
  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_ERR_CONFLICT);
  f12_stat_t stat;
  ASSERT_EQ(f12_readdir(&dir, &stat), DISK_OK);
  ASSERT_EQ(f12_closedir(&dir), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_fsck_conflict_and_clean) {
  prepare();
  fat12_fsck_t report;
  ASSERT_EQ(f12_fsck(&fs, &report, false), DISK_OK);
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "FSCK.BIN", F12_OPEN_WRITE, &file), DISK_OK);
  ASSERT_EQ(f12_fsck(&fs, &report, false), DISK_ERR_CONFLICT);
  ASSERT_EQ(f12_fsck(&fs, &report, true), DISK_ERR_CONFLICT);
  ASSERT_EQ(f12_abort(&file), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_mount_reports_device_errors_verbatim) {
  static const disk_err_t cases[] = {
      DISK_ERR_TIMEOUT, DISK_ERR_BUSY, DISK_ERR_CRC, DISK_ERR_WRONG_TRACK,
      DISK_ERR_WRONG_SIDE, DISK_ERR_NO_TRACK0, DISK_ERR_MEDIA_CHANGED,
      DISK_ERR_WRITE_PROTECTED, DISK_ERR_UNDERRUN, DISK_ERR_OVERRUN,
      DISK_ERR_VERIFY, DISK_ERR_CORRUPT, DISK_ERR_IO,
  };
  vdisk_init(&disk);
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  ASSERT_EQ(f12_format(&fs, format_options("ERRORS", F12_FORMAT_QUICK)), DISK_OK);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
    disk.read_status = cases[i];
    ASSERT_EQ(f12_mount(&fs), cases[i]);
    disk.read_status = DISK_OK;
  }
}

TEST(test_format_device_error_is_verbatim) {
  vdisk_init(&disk);
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  disk.write_status = DISK_ERR_VERIFY;
  ASSERT_EQ(f12_format(&fs, format_options("FAIL", F12_FORMAT_FULL)),
            DISK_ERR_VERIFY);
}

TEST(test_slot_generations_skip_zero_on_wrap) {
  prepare();
  fs.files[0].generation = UINT64_MAX;
  f12_file_t file;
  ASSERT_EQ(f12_open(&fs, "WRAP.BIN", F12_OPEN_WRITE, &file), DISK_OK);
  ASSERT_EQ(file.token.slot, 0);
  ASSERT_EQ(file.token.slot_generation, 1);
  ASSERT_EQ(f12_abort(&file), DISK_OK);

  fs.dirs[0].generation = UINT64_MAX;
  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), DISK_OK);
  ASSERT_EQ(dir.token.slot, 0);
  ASSERT_EQ(dir.token.slot_generation, 1);
  ASSERT_EQ(f12_closedir(&dir), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_strerror_is_total) {
  for (int error = DISK_OK; error <= DISK_ERR_LAST; error++) {
    ASSERT(strcmp(disk_strerror((disk_err_t)error), "unknown error") != 0);
  }
  ASSERT_STR_EQ(disk_strerror((disk_err_t)(DISK_ERR_LAST + 1)), "unknown error");
}

int main(void) {
  printf("=== F12 Contract Tests ===\n\n");
  RUN_TEST(test_init_contract);
  RUN_TEST(test_format_mount_unmount);
  RUN_TEST(test_format_options_and_progress_context);
  RUN_TEST(test_quick_format_needs_no_reads_from_blank_media);
  RUN_TEST(test_mount_adopts_media_changed_since_the_last_look);
  RUN_TEST(test_format_adopts_media_changed_only_before_the_first_track);
  RUN_TEST(test_read_only_device_mounts);
  RUN_TEST(test_corrupt_full_track_rejected);
  RUN_TEST(test_file_roundtrip_and_end);
  RUN_TEST(test_seek_tell_and_read_at);
  RUN_TEST(test_mode_errors_are_typed);
  RUN_TEST(test_stale_file_handle_generation);
  RUN_TEST(test_handles_do_not_survive_remount);
  RUN_TEST(test_media_generation_invalidates_mount);
  RUN_TEST(test_tell_observes_media_generation);
  RUN_TEST(test_reinit_does_not_resurrect_handles);
  RUN_TEST(test_live_reinit_resets_and_invalidates_handles);
  RUN_TEST(test_invalid_reinit_preserves_live_context);
  RUN_TEST(test_media_change_forgets_commit_phase_writer);
  RUN_TEST(test_open_conflicts);
  RUN_TEST(test_multiple_readers_and_limit);
  RUN_TEST(test_directory_end_and_generation);
  RUN_TEST(test_readdir_preserves_io_error);
  RUN_TEST(test_list_propagates_callback_error);
  RUN_TEST(test_list_visits_every_entry_and_releases_its_directory);
  RUN_TEST(test_tell_follows_writer_progress);
  RUN_TEST(test_read_at_zero_length_leaves_position_untouched);
  RUN_TEST(test_delete_rename_and_stat);
  RUN_TEST(test_free_count_is_typed_and_transaction_safe);
  RUN_TEST(test_write_protection);
  RUN_TEST(test_generation_observer_errors_are_exact);
  RUN_TEST(test_write_protection_observer_errors_are_exact);
  RUN_TEST(test_abort_discards_uncommitted_writer);
  RUN_TEST(test_failed_close_is_retryable_and_not_abortable);
  RUN_TEST(test_unmount_preserves_open_handle_ownership);
  RUN_TEST(test_fsck_conflict_and_clean);
  RUN_TEST(test_mount_reports_device_errors_verbatim);
  RUN_TEST(test_format_device_error_is_verbatim);
  RUN_TEST(test_slot_generations_skip_zero_on_wrap);
  RUN_TEST(test_strerror_is_total);
  TEST_RESULTS();
}
