#include "f12.h"
#include <string.h>

#define F12_SIGNATURE 0x46313246u

static uint64_t f12_next_generation(uint64_t generation) {
  generation++;
  return generation ? generation : 1u;
}

static uint64_t f12_incarnation_source;

static uint64_t f12_next_incarnation(void) {
  f12_incarnation_source++;
  if (f12_incarnation_source == 0) f12_incarnation_source++;
  return f12_incarnation_source;
}

static bool f12_valid(const f12_t *fs) {
  return fs && fs->signature == F12_SIGNATURE &&
         fs->signature_inverse == ~F12_SIGNATURE;
}

static disk_result_t f12_result(disk_err_t error, size_t count) {
  return (disk_result_t){.error = error, .count = count};
}

static void f12_release_file_slot(f12_file_slot_t *slot) {
  slot->active = false;
  slot->mode = 0;
  slot->generation = f12_next_generation(slot->generation);
  memset(&slot->name, 0, sizeof(slot->name));
  memset(&slot->io, 0, sizeof(slot->io));
}

static void f12_release_dir_slot(f12_dir_slot_t *slot) {
  slot->active = false;
  slot->index = 0;
  slot->generation = f12_next_generation(slot->generation);
}

static void f12_invalidate(f12_t *fs) {
  for (size_t i = 0; i < F12_MAX_OPEN_FILES; i++) {
    f12_file_slot_t *slot = &fs->files[i];
    if (slot->active && slot->mode == F12_OPEN_WRITE) {
      fat12_forget_write(&slot->io.writer);
    }
    f12_release_file_slot(slot);
  }
  for (size_t i = 0; i < F12_MAX_OPEN_DIRS; i++) {
    f12_release_dir_slot(&fs->dirs[i]);
  }
  cache_clear(&fs->cache);
  fs->state = F12_STATE_READY;
}

static disk_err_t f12_finish(f12_t *fs, disk_err_t error) {
  if (error == DISK_ERR_MEDIA_CHANGED && f12_valid(fs) &&
      fs->state == F12_STATE_MOUNTED) {
    f12_invalidate(fs);
  }
  return error;
}

static disk_err_t f12_check_mounted(f12_t *fs) {
  if (!f12_valid(fs)) return DISK_ERR_NOT_INITIALIZED;
  if (fs->state != F12_STATE_MOUNTED) return DISK_ERR_NOT_MOUNTED;
  return f12_finish(fs, cache_check(&fs->cache));
}

static disk_err_t f12_check_writable(f12_t *fs) {
  disk_err_t error = f12_check_mounted(fs);
  if (error != DISK_OK) return error;
  return f12_finish(fs, cache_writable(&fs->cache));
}

static bool f12_writer_open(const f12_t *fs) {
  for (size_t i = 0; i < F12_MAX_OPEN_FILES; i++) {
    if (fs->files[i].active && fs->files[i].mode == F12_OPEN_WRITE) return true;
  }
  return false;
}

static bool f12_operations_open(const f12_t *fs) {
  for (size_t i = 0; i < F12_MAX_OPEN_FILES; i++) {
    if (fs->files[i].active) return true;
  }
  for (size_t i = 0; i < F12_MAX_OPEN_DIRS; i++) {
    if (fs->dirs[i].active) return true;
  }
  return false;
}

static bool f12_name_open(const f12_t *fs, const fat12_name_t *name) {
  for (size_t i = 0; i < F12_MAX_OPEN_FILES; i++) {
    if (fs->files[i].active &&
        memcmp(&fs->files[i].name, name, sizeof(*name)) == 0) {
      return true;
    }
  }
  return false;
}

static f12_file_slot_t *f12_file_slot(f12_file_t *file) {
  if (!file || !f12_valid(file->token.fs) ||
      file->token.fs->state != F12_STATE_MOUNTED ||
      file->token.slot >= F12_MAX_OPEN_FILES ||
      file->token.incarnation != file->token.fs->incarnation) {
    return NULL;
  }
  f12_file_slot_t *slot = &file->token.fs->files[file->token.slot];
  if (!slot->active || slot->generation != file->token.slot_generation) {
    return NULL;
  }
  return slot;
}

static f12_dir_slot_t *f12_dir_slot(f12_dir_t *dir) {
  if (!dir || !f12_valid(dir->token.fs) ||
      dir->token.fs->state != F12_STATE_MOUNTED ||
      dir->token.slot >= F12_MAX_OPEN_DIRS ||
      dir->token.incarnation != dir->token.fs->incarnation) {
    return NULL;
  }
  f12_dir_slot_t *slot = &dir->token.fs->dirs[dir->token.slot];
  if (!slot->active || slot->generation != dir->token.slot_generation) {
    return NULL;
  }
  return slot;
}

static const char *f12_path(const char *path) {
  return path && path[0] == '/' ? path + 1 : path;
}

static void f12_fill_stat(const fat12_dirent_t *entry, f12_stat_t *stat) {
  size_t pos = 0;
  size_t i = 0;
  while (i < FAT12_FILENAME_LEN && entry->name[i] != ' ') {
    stat->name[pos++] = entry->name[i++];
  }
  if (entry->ext[0] != ' ') {
    stat->name[pos++] = '.';
    i = 0;
    while (i < FAT12_EXTENSION_LEN && entry->ext[i] != ' ') {
      stat->name[pos++] = entry->ext[i++];
    }
  }
  stat->name[pos] = '\0';
  stat->size = entry->size;
  stat->attr = entry->attr;
}

disk_err_t f12_init(f12_t *fs, disk_device_t device) {
  if (!fs || !device.read_track || !device.media_generation ||
      !device.write_protected) {
    return DISK_ERR_INVALID;
  }
  uint64_t incarnation = f12_next_incarnation();
  memset(fs, 0, sizeof(*fs));
  cache_init(&fs->cache, device);
  fs->incarnation = incarnation;
  fs->signature = F12_SIGNATURE;
  fs->signature_inverse = ~F12_SIGNATURE;
  for (size_t i = 0; i < F12_MAX_OPEN_FILES; i++) fs->files[i].generation = 1;
  for (size_t i = 0; i < F12_MAX_OPEN_DIRS; i++) fs->dirs[i].generation = 1;
  return DISK_OK;
}

static disk_err_t f12_read_superblock(f12_t *fs) {
  disk_err_t status = cache_bind(&fs->cache);
  if (status != DISK_OK) return status;
  return fat12_init(&fs->fat, &fs->cache);
}

disk_err_t f12_mount(f12_t *fs) {
  if (!f12_valid(fs)) return DISK_ERR_NOT_INITIALIZED;
  if (fs->state == F12_STATE_MOUNTED) return DISK_ERR_ALREADY_MOUNTED;
  if (fs->state != F12_STATE_READY) return DISK_ERR_BUSY;
  if (f12_operations_open(fs) || fat12_busy(&fs->fat)) return DISK_ERR_CONFLICT;

  fs->state = F12_STATE_BLOCK_ACCESS;
  disk_err_t status = f12_read_superblock(fs);
  if (status == DISK_ERR_MEDIA_CHANGED) status = f12_read_superblock(fs);
  if (status == DISK_OK) status = cache_check(&fs->cache);
  fs->state = F12_STATE_READY;
  if (status != DISK_OK) {
    cache_clear(&fs->cache);
    return status;
  }
  fs->state = F12_STATE_MOUNTED;
  return DISK_OK;
}

disk_err_t f12_unmount(f12_t *fs) {
  disk_err_t error = f12_check_mounted(fs);
  if (error != DISK_OK) return error;
  if (f12_operations_open(fs)) return DISK_ERR_CONFLICT;
  if (fat12_busy(&fs->fat)) return DISK_ERR_BUSY;
  cache_clear(&fs->cache);
  fs->state = F12_STATE_READY;
  return DISK_OK;
}

typedef struct {
  f12_format_options_t options;
  uint16_t done;
} f12_format_progress_t;

static void f12_format_progress(void *ctx, uint8_t cylinder, uint8_t head,
                                uint16_t done, uint16_t total) {
  f12_format_progress_t *progress = ctx;
  progress->done = done;
  if (progress->options.progress) {
    progress->options.progress(progress->options.progress_ctx, cylinder, head,
                               done, total);
  }
}

static disk_err_t f12_write_format(f12_t *fs, f12_format_progress_t *progress) {
  disk_err_t status = cache_bind(&fs->cache);
  if (status != DISK_OK) return status;
  return fat12_format(&fs->fat, &fs->cache, progress->options.label,
                      progress->options.mode == F12_FORMAT_FULL,
                      f12_format_progress, progress);
}

disk_err_t f12_format(f12_t *fs, f12_format_options_t options) {
  if (!f12_valid(fs)) return DISK_ERR_NOT_INITIALIZED;
  if (!options.label ||
      (options.mode != F12_FORMAT_QUICK && options.mode != F12_FORMAT_FULL)) {
    return DISK_ERR_INVALID;
  }
  if (fs->state == F12_STATE_MOUNTED) return DISK_ERR_ALREADY_MOUNTED;
  if (fs->state != F12_STATE_READY) return DISK_ERR_BUSY;
  if (f12_operations_open(fs) || fat12_busy(&fs->fat)) return DISK_ERR_CONFLICT;
  disk_err_t status = cache_writable(&fs->cache);
  if (status != DISK_OK) return status;
  fs->state = F12_STATE_BLOCK_ACCESS;
  f12_format_progress_t progress = {.options = options};
  status = f12_write_format(fs, &progress);
  if (status == DISK_ERR_MEDIA_CHANGED && progress.done == 0) {
    status = f12_write_format(fs, &progress);
  }
  if (status == DISK_OK) status = cache_check(&fs->cache);
  fs->state = F12_STATE_READY;
  return status;
}

disk_err_t f12_fsck(f12_t *fs, fat12_fsck_t *report, bool repair) {
  if (!report) return DISK_ERR_INVALID;
  disk_err_t error = repair ? f12_check_writable(fs) : f12_check_mounted(fs);
  if (error != DISK_OK) return error;
  if ((repair && f12_operations_open(fs)) || f12_writer_open(fs) ||
      fat12_busy(&fs->fat)) {
    return DISK_ERR_CONFLICT;
  }
  return f12_finish(fs, fat12_fsck(&fs->fat, report, repair));
}

disk_err_t f12_is_mounted(f12_t *fs, bool *mounted) {
  if (mounted) *mounted = false;
  if (!mounted) return DISK_ERR_INVALID;
  if (!f12_valid(fs)) return DISK_ERR_NOT_INITIALIZED;
  if (fs->state == F12_STATE_READY) return DISK_OK;
  if (fs->state != F12_STATE_MOUNTED) return DISK_ERR_BUSY;
  disk_err_t error = f12_check_mounted(fs);
  if (error == DISK_OK) *mounted = true;
  return error;
}

disk_err_t f12_open(f12_t *fs, const char *path, f12_open_mode_t mode,
                    f12_file_t *out) {
  if (out) memset(out, 0, sizeof(*out));
  if (!out || !path || (mode != F12_OPEN_READ && mode != F12_OPEN_WRITE)) {
    return DISK_ERR_INVALID;
  }
  disk_err_t error = mode == F12_OPEN_WRITE ? f12_check_writable(fs)
                                            : f12_check_mounted(fs);
  if (error != DISK_OK) return error;
  const char *name = f12_path(path);
  fat12_name_t parsed;
  error = fat12_name_parse(name, &parsed);
  if (error != DISK_OK) return error;
  if ((mode == F12_OPEN_READ && f12_writer_open(fs) && f12_name_open(fs, &parsed)) ||
      (mode == F12_OPEN_WRITE &&
       (f12_writer_open(fs) || fat12_busy(&fs->fat) || f12_name_open(fs, &parsed)))) {
    return DISK_ERR_CONFLICT;
  }

  size_t index = F12_MAX_OPEN_FILES;
  for (size_t i = 0; i < F12_MAX_OPEN_FILES; i++) {
    if (!fs->files[i].active) {
      index = i;
      break;
    }
  }
  if (index == F12_MAX_OPEN_FILES) return DISK_ERR_TOO_MANY;

  f12_file_slot_t *slot = &fs->files[index];
  if (mode == F12_OPEN_READ) {
    fat12_dirent_t entry;
    error = fat12_find(&fs->fat, name, &entry);
    if (error != DISK_OK) return f12_finish(fs, error);
    if ((entry.attr & FAT12_ATTR_DIRECTORY) != 0) return DISK_ERR_IS_DIR;
    error = fat12_open(&fs->fat, &entry, &slot->io.reader);
  } else {
    error = fat12_open_write(&fs->fat, name, &slot->io.writer);
  }
  if (error != DISK_OK) return f12_finish(fs, error);

  slot->generation = f12_next_generation(slot->generation);
  slot->name = parsed;
  slot->mode = mode;
  slot->active = true;
  *out = (f12_file_t){.token = {
      .fs = fs,
      .incarnation = fs->incarnation,
      .slot_generation = slot->generation,
      .slot = (uint16_t)index,
  }};
  return DISK_OK;
}

disk_err_t f12_close(f12_file_t *file) {
  f12_file_slot_t *slot = f12_file_slot(file);
  if (!slot) return DISK_ERR_BAD_HANDLE;
  f12_t *fs = file->token.fs;
  disk_err_t error = f12_check_mounted(fs);
  if (error != DISK_OK) return error;
  if (slot->mode == F12_OPEN_WRITE) {
    error = fat12_close_write(&slot->io.writer);
    if (error != DISK_OK) return f12_finish(fs, error);
  }
  f12_release_file_slot(slot);
  memset(file, 0, sizeof(*file));
  return DISK_OK;
}

disk_err_t f12_abort(f12_file_t *file) {
  f12_file_slot_t *slot = f12_file_slot(file);
  if (!slot) return DISK_ERR_BAD_HANDLE;
  if (slot->mode != F12_OPEN_WRITE) return DISK_ERR_CONFLICT;
  disk_err_t error = f12_check_mounted(file->token.fs);
  if (error != DISK_OK) return error;
  error = fat12_abort_write(&slot->io.writer);
  if (error != DISK_OK) return error;
  f12_release_file_slot(slot);
  memset(file, 0, sizeof(*file));
  return DISK_OK;
}

disk_result_t f12_read(f12_file_t *file, void *buf, size_t len) {
  f12_file_slot_t *slot = f12_file_slot(file);
  if (!slot) return f12_result(DISK_ERR_BAD_HANDLE, 0);
  if (slot->mode != F12_OPEN_READ) return f12_result(DISK_ERR_CONFLICT, 0);
  if (!buf && len != 0) return f12_result(DISK_ERR_INVALID, 0);
  if (len == 0) return f12_result(DISK_OK, 0);
  disk_err_t error = f12_check_mounted(file->token.fs);
  if (error != DISK_OK) return f12_result(error, 0);
  disk_result_t result = fat12_read(&slot->io.reader, (uint8_t *)buf, len);
  error = f12_finish(file->token.fs, result.error);
  if (error != DISK_OK) return f12_result(error, result.count);
  return f12_result(result.count == 0 ? DISK_END : DISK_OK, result.count);
}

disk_result_t f12_write(f12_file_t *file, const void *buf, size_t len) {
  f12_file_slot_t *slot = f12_file_slot(file);
  if (!slot) return f12_result(DISK_ERR_BAD_HANDLE, 0);
  if (slot->mode != F12_OPEN_WRITE) return f12_result(DISK_ERR_CONFLICT, 0);
  if (!buf && len != 0) return f12_result(DISK_ERR_INVALID, 0);
  if (len == 0) return f12_result(DISK_OK, 0);
  disk_err_t error = f12_check_writable(file->token.fs);
  if (error != DISK_OK) return f12_result(error, 0);
  disk_result_t result = fat12_write(&slot->io.writer, (const uint8_t *)buf, len);
  error = f12_finish(file->token.fs, result.error);
  if (error != DISK_OK) return f12_result(error, result.count);
  if (result.count == 0) return f12_result(DISK_ERR_IO, 0);
  return f12_result(DISK_OK, result.count);
}

disk_err_t f12_seek(f12_file_t *file, uint32_t offset) {
  f12_file_slot_t *slot = f12_file_slot(file);
  if (!slot) return DISK_ERR_BAD_HANDLE;
  if (slot->mode != F12_OPEN_READ) return DISK_ERR_CONFLICT;
  disk_err_t error = f12_check_mounted(file->token.fs);
  if (error != DISK_OK) return error;
  return f12_finish(file->token.fs, fat12_seek(&slot->io.reader, offset));
}

disk_err_t f12_tell(const f12_file_t *file, uint32_t *offset) {
  if (!offset) return DISK_ERR_INVALID;
  f12_file_slot_t *slot = f12_file_slot((f12_file_t *)file);
  if (!slot) return DISK_ERR_BAD_HANDLE;
  disk_err_t error = f12_check_mounted(file->token.fs);
  if (error != DISK_OK) return error;
  *offset = slot->mode == F12_OPEN_READ ? slot->io.reader.bytes_read
                                        : slot->io.writer.bytes_written;
  return DISK_OK;
}

disk_result_t f12_read_at(f12_file_t *file, uint32_t offset, void *buf,
                          size_t len) {
  f12_file_slot_t *slot = f12_file_slot(file);
  if (!slot) return f12_result(DISK_ERR_BAD_HANDLE, 0);
  if (slot->mode != F12_OPEN_READ) return f12_result(DISK_ERR_CONFLICT, 0);
  if (!buf && len != 0) return f12_result(DISK_ERR_INVALID, 0);
  f12_t *fs = file->token.fs;
  disk_err_t error = f12_check_mounted(fs);
  if (error != DISK_OK) return f12_result(error, 0);
  fat12_file_t reader = slot->io.reader;
  error = fat12_seek(&reader, offset);
  if (error != DISK_OK) return f12_result(f12_finish(fs, error), 0);
  if (len == 0) return f12_result(DISK_OK, 0);
  disk_result_t result = fat12_read(&reader, (uint8_t *)buf, len);
  error = f12_finish(fs, result.error);
  if (error != DISK_OK) return f12_result(error, result.count);
  return f12_result(result.count == 0 ? DISK_END : DISK_OK, result.count);
}

disk_err_t f12_stat(f12_t *fs, const char *path, f12_stat_t *stat) {
  if (stat) memset(stat, 0, sizeof(*stat));
  if (!path || !stat) return DISK_ERR_INVALID;
  disk_err_t error = f12_check_mounted(fs);
  if (error != DISK_OK) return error;
  fat12_dirent_t entry;
  error = fat12_find(&fs->fat, f12_path(path), &entry);
  if (error != DISK_OK) return f12_finish(fs, error);
  f12_fill_stat(&entry, stat);
  return DISK_OK;
}

disk_err_t f12_free_count(f12_t *fs, uint16_t *count) {
  if (count) *count = 0;
  if (!count) return DISK_ERR_INVALID;
  disk_err_t error = f12_check_mounted(fs);
  if (error != DISK_OK) return error;
  if (f12_writer_open(fs) || fat12_busy(&fs->fat)) return DISK_ERR_CONFLICT;
  return f12_finish(fs, fat12_free_count(&fs->fat, count));
}

disk_err_t f12_delete(f12_t *fs, const char *path) {
  if (!path) return DISK_ERR_INVALID;
  disk_err_t error = f12_check_writable(fs);
  if (error != DISK_OK) return error;
  if (f12_operations_open(fs) || fat12_busy(&fs->fat)) return DISK_ERR_CONFLICT;
  return f12_finish(fs, fat12_delete(&fs->fat, f12_path(path)));
}

disk_err_t f12_rename(f12_t *fs, const char *from, const char *to) {
  if (!from || !to) return DISK_ERR_INVALID;
  disk_err_t error = f12_check_writable(fs);
  if (error != DISK_OK) return error;
  if (f12_operations_open(fs) || fat12_busy(&fs->fat)) return DISK_ERR_CONFLICT;
  return f12_finish(fs, fat12_rename(&fs->fat, f12_path(from), f12_path(to)));
}

disk_err_t f12_opendir(f12_t *fs, const char *path, f12_dir_t *dir) {
  if (dir) memset(dir, 0, sizeof(*dir));
  if (!path || !dir) return DISK_ERR_INVALID;
  disk_err_t error = f12_check_mounted(fs);
  if (error != DISK_OK) return error;
  if (*f12_path(path) != '\0') return DISK_ERR_NOT_DIR;
  if (f12_writer_open(fs) || fat12_busy(&fs->fat)) return DISK_ERR_CONFLICT;

  size_t index = F12_MAX_OPEN_DIRS;
  for (size_t i = 0; i < F12_MAX_OPEN_DIRS; i++) {
    if (!fs->dirs[i].active) {
      index = i;
      break;
    }
  }
  if (index == F12_MAX_OPEN_DIRS) return DISK_ERR_TOO_MANY;

  f12_dir_slot_t *slot = &fs->dirs[index];
  slot->generation = f12_next_generation(slot->generation);
  slot->index = 0;
  slot->active = true;
  *dir = (f12_dir_t){.token = {
      .fs = fs,
      .incarnation = fs->incarnation,
      .slot_generation = slot->generation,
      .slot = (uint16_t)index,
  }};
  return DISK_OK;
}

disk_err_t f12_readdir(f12_dir_t *dir, f12_stat_t *stat) {
  if (stat) memset(stat, 0, sizeof(*stat));
  if (!stat) return DISK_ERR_INVALID;
  f12_dir_slot_t *slot = f12_dir_slot(dir);
  if (!slot) return DISK_ERR_BAD_HANDLE;
  f12_t *fs = dir->token.fs;
  disk_err_t error = f12_check_mounted(fs);
  if (error != DISK_OK) return error;
  for (;;) {
    fat12_dirent_t entry;
    error = fat12_read_root_entry(&fs->fat, slot->index, &entry);
    if (error != DISK_OK) return f12_finish(fs, error);
    slot->index++;
    if (fat12_entry_is_end(&entry)) return DISK_END;
    if (!fat12_entry_valid(&entry) || (entry.attr & FAT12_ATTR_VOLUME_ID) != 0) {
      continue;
    }
    f12_fill_stat(&entry, stat);
    return DISK_OK;
  }
}

disk_err_t f12_closedir(f12_dir_t *dir) {
  f12_dir_slot_t *slot = f12_dir_slot(dir);
  if (!slot) return DISK_ERR_BAD_HANDLE;
  f12_release_dir_slot(slot);
  memset(dir, 0, sizeof(*dir));
  return DISK_OK;
}

disk_err_t f12_list(f12_t *fs, f12_list_fn fn, void *ctx) {
  if (!fn) return DISK_ERR_INVALID;
  f12_dir_t dir;
  disk_err_t error = f12_opendir(fs, "/", &dir);
  if (error != DISK_OK) return error;
  for (;;) {
    f12_stat_t stat;
    error = f12_readdir(&dir, &stat);
    if (error == DISK_END) return f12_closedir(&dir);
    if (error == DISK_OK) error = fn(ctx, &stat);
    if (error != DISK_OK) {
      f12_closedir(&dir);
      return error;
    }
  }
}
