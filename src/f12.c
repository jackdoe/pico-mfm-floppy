#include "f12.h"
#include <string.h>

// ============== Internal Helpers ==============

// Set error and return it
static f12_err_t f12_set_error(f12_t *fs, f12_err_t err) {
  if (fs) fs->last_error = err;
  return err;
}

// Check if disk was changed
static f12_err_t f12_check_disk(f12_t *fs) {
  if (!fs->mounted) {
    return f12_set_error(fs, F12_ERR_NOT_MOUNTED);
  }

  if (fs->io.disk_changed && fs->io.disk_changed(fs->io.ctx)) {
    // Clear cache
    if (fs->cache) {
      lru_clear(fs->cache);
    }

    // Invalidate all open files
    for (int i = 0; i < F12_MAX_OPEN_FILES; i++) {
      fs->files[i].mode = F12_MODE_CLOSED;
    }

    fs->mounted = false;
    return f12_set_error(fs, F12_ERR_DISK_CHANGED);
  }

  return F12_OK;
}

// Check if disk is writable
static f12_err_t f12_check_writable(f12_t *fs) {
  f12_err_t err = f12_check_disk(fs);
  if (err != F12_OK) return err;

  if (fs->io.write_protected && fs->io.write_protected(fs->io.ctx)) {
    return f12_set_error(fs, F12_ERR_WRITE_PROTECTED);
  }

  return F12_OK;
}

// Cached sector read callback for fat12
static bool f12_cached_read(void *ctx, sector_t *sector) {
  f12_t *fs = (f12_t *)ctx;

  // Check disk change (but only if mounted - during mount we haven't set the flag yet)
  if (fs->mounted) {
    if (f12_check_disk(fs) != F12_OK) {
      return false;
    }
  }

  // Try cache first
  uint32_t key = lru_key(sector->track, sector->side, sector->sector_n);
  uint8_t *cached = lru_get(fs->cache, key);
  if (cached) {
    memcpy(sector->data, cached, SECTOR_SIZE);
    sector->valid = true;
    return true;
  }

  // Cache miss - read from disk
  if (!fs->io.read(fs->io.ctx, sector)) {
    return false;
  }

  // Add to cache if valid
  if (sector->valid) {
    lru_set(fs->cache, key, sector->data);
  }

  return true;
}

// Cached write callback for fat12
static bool f12_cached_write(void *ctx, track_t *track) {
  f12_t *fs = (f12_t *)ctx;

  // Check writable (but only if mounted - during format we haven't set the flag yet)
  if (fs->mounted) {
    if (f12_check_writable(fs) != F12_OK) {
      return false;
    }
  }

  // Write to disk
  if (!fs->io.write(fs->io.ctx, track)) {
    return false;
  }

  // Update cache with written sectors
  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    if (track->sectors[i].valid) {
      uint32_t key = lru_key(track->track, track->side, track->sectors[i].sector_n);
      lru_set(fs->cache, key, track->sectors[i].data);
    }
  }

  return true;
}

// Find free file slot
static f12_file_t *f12_alloc_file(f12_t *fs) {
  for (int i = 0; i < F12_MAX_OPEN_FILES; i++) {
    if (fs->files[i].mode == F12_MODE_CLOSED) {
      f12_file_t *f = &fs->files[i];
      memset(f, 0, sizeof(*f));
      f->fs = fs;
      return f;
    }
  }
  return NULL;
}

// Convert fat12 error to f12 error
static f12_err_t fat12_to_f12_err(fat12_err_t err) {
  switch (err) {
    case FAT12_OK:        return F12_OK;
    case FAT12_ERR_READ:  return F12_ERR_IO;
    case FAT12_ERR_WRITE: return F12_ERR_IO;
    case FAT12_ERR_INVALID: return F12_ERR_INVALID;
    case FAT12_ERR_NOT_FOUND: return F12_ERR_NOT_FOUND;
    case FAT12_ERR_EOF:   return F12_ERR_EOF;
    case FAT12_ERR_FULL:  return F12_ERR_FULL;
    default:              return F12_ERR_IO;
  }
}

// Format dirent name to 8.3 string
static void format_name_83(const fat12_dirent_t *entry, char *out) {
  int i, j = 0;

  // Copy name, trim trailing spaces
  for (i = 0; i < 8 && entry->name[i] != ' '; i++) {
    out[j++] = entry->name[i];
  }

  // Add dot and extension if present
  if (entry->ext[0] != ' ') {
    out[j++] = '.';
    for (i = 0; i < 3 && entry->ext[i] != ' '; i++) {
      out[j++] = entry->ext[i];
    }
  }

  out[j] = '\0';
}

// ============== Lifecycle ==============

f12_err_t f12_mount(f12_t *fs, f12_io_t io) {
  if (!fs) return F12_ERR_INVALID;

  memset(fs, 0, sizeof(*fs));
  fs->io = io;

  // Create cache
  fs->cache = lru_init(F12_CACHE_SIZE, SECTOR_SIZE);
  if (!fs->cache) {
    return f12_set_error(fs, F12_ERR_IO);
  }

  // Setup fat12 io with caching
  fat12_io_t fat_io = {
    .read = f12_cached_read,
    .write = f12_cached_write,
    .ctx = fs,
  };

  // Initialize FAT12
  fat12_err_t err = fat12_init(&fs->fat, fat_io);
  if (err != FAT12_OK) {
    lru_free(fs->cache);
    fs->cache = NULL;
    return f12_set_error(fs, fat12_to_f12_err(err));
  }

  fs->mounted = true;
  return F12_OK;
}

void f12_unmount(f12_t *fs) {
  if (!fs) return;

  // Close all open files
  for (int i = 0; i < F12_MAX_OPEN_FILES; i++) {
    if (fs->files[i].mode != F12_MODE_CLOSED) {
      f12_close(&fs->files[i]);
    }
  }

  // Free cache
  if (fs->cache) {
    lru_free(fs->cache);
    fs->cache = NULL;
  }

  fs->mounted = false;
}

f12_err_t f12_format(f12_t *fs, const char *label, bool full) {
  if (!fs) return F12_ERR_INVALID;

  // Check writable (but not mounted - we're formatting)
  if (fs->io.write_protected && fs->io.write_protected(fs->io.ctx)) {
    return f12_set_error(fs, F12_ERR_WRITE_PROTECTED);
  }

  // Create fat12 io for format (no caching needed)
  fat12_io_t fat_io = {
    .read = fs->io.read,
    .write = fs->io.write,
    .ctx = fs->io.ctx,
  };

  // Format
  fat12_err_t err = fat12_format(fat_io, label, full);
  if (err != FAT12_OK) {
    return f12_set_error(fs, fat12_to_f12_err(err));
  }

  // Clear cache after format
  if (fs->cache) {
    lru_clear(fs->cache);
  }

  // Re-mount to read new boot sector
  if (fs->mounted) {
    fs->mounted = false;
    return f12_mount(fs, fs->io);
  }

  return F12_OK;
}

// ============== File Operations ==============

f12_file_t *f12_open(f12_t *fs, const char *path, const char *mode) {
  if (!fs || !path || !mode) {
    f12_set_error(fs, F12_ERR_INVALID);
    return NULL;
  }

  // Parse mode
  f12_file_mode_t fmode;
  bool truncate = false;

  if (mode[0] == 'r') {
    fmode = F12_MODE_READ;
  } else if (mode[0] == 'w') {
    fmode = F12_MODE_WRITE;
    truncate = true;
  } else if (mode[0] == 'a') {
    fmode = F12_MODE_WRITE;
  } else {
    f12_set_error(fs, F12_ERR_INVALID);
    return NULL;
  }

  // Check disk (and writable for write modes)
  f12_err_t err;
  if (fmode == F12_MODE_WRITE) {
    err = f12_check_writable(fs);
  } else {
    err = f12_check_disk(fs);
  }
  if (err != F12_OK) {
    return NULL;
  }

  // Skip leading slash if present
  if (path[0] == '/') path++;

  // Allocate file slot
  f12_file_t *file = f12_alloc_file(fs);
  if (!file) {
    f12_set_error(fs, F12_ERR_TOO_MANY);
    return NULL;
  }

  if (fmode == F12_MODE_READ) {
    // Find and open for reading
    fat12_err_t ferr = fat12_find(&fs->fat, path, &file->dirent);
    if (ferr != FAT12_OK) {
      f12_set_error(fs, fat12_to_f12_err(ferr));
      return NULL;
    }

    // Check if it's a directory
    if (file->dirent.attr & FAT12_ATTR_DIRECTORY) {
      f12_set_error(fs, F12_ERR_IS_DIR);
      return NULL;
    }

    // Open for reading
    ferr = fat12_open(&fs->fat, &file->dirent, &file->reader);
    if (ferr != FAT12_OK) {
      f12_set_error(fs, fat12_to_f12_err(ferr));
      return NULL;
    }

    file->mode = F12_MODE_READ;
    file->position = 0;

  } else {
    // Write mode - open or create
    fat12_err_t ferr;

    if (truncate) {
      // Delete existing file first (ignore not found error)
      fat12_delete(&fs->fat, path);
    }

    // Open for writing (creates if doesn't exist)
    ferr = fat12_open_write(&fs->fat, path, &file->writer);
    if (ferr != FAT12_OK) {
      f12_set_error(fs, fat12_to_f12_err(ferr));
      return NULL;
    }

    file->mode = F12_MODE_WRITE;
    file->position = 0;

    // For append mode, seek to end would be needed
    // But our fat12_writer always appends, so position tracking is for user reference
    if (mode[0] == 'a') {
      file->position = file->writer.bytes_written;
    }
  }

  return file;
}

f12_err_t f12_close(f12_file_t *file) {
  if (!file || !file->fs) {
    return F12_ERR_BAD_HANDLE;
  }

  f12_t *fs = file->fs;

  if (file->mode == F12_MODE_WRITE) {
    // Flush writes
    fat12_err_t ferr = fat12_close_write(&file->writer);
    if (ferr != FAT12_OK) {
      file->mode = F12_MODE_CLOSED;
      return f12_set_error(fs, fat12_to_f12_err(ferr));
    }
  }

  file->mode = F12_MODE_CLOSED;
  return F12_OK;
}

int f12_read(f12_file_t *file, void *buf, size_t len) {
  if (!file || !file->fs || !buf) return -1;

  if (file->mode != F12_MODE_READ) {
    f12_set_error(file->fs, F12_ERR_INVALID);
    return -1;
  }

  // Check disk
  if (f12_check_disk(file->fs) != F12_OK) {
    return -1;
  }

  int n = fat12_read(&file->reader, buf, len);
  if (n < 0) {
    f12_set_error(file->fs, F12_ERR_IO);
    return -1;
  }

  file->position += n;
  return n;
}

int f12_write(f12_file_t *file, const void *buf, size_t len) {
  if (!file || !file->fs || !buf) return -1;

  if (file->mode != F12_MODE_WRITE) {
    f12_set_error(file->fs, F12_ERR_INVALID);
    return -1;
  }

  // Check writable
  if (f12_check_writable(file->fs) != F12_OK) {
    return -1;
  }

  int n = fat12_write(&file->writer, buf, len);
  if (n < 0) {
    f12_set_error(file->fs, F12_ERR_IO);
    return -1;
  }

  file->position += n;
  return n;
}

f12_err_t f12_seek(f12_file_t *file, uint32_t offset) {
  if (!file || !file->fs) return F12_ERR_BAD_HANDLE;

  if (file->mode != F12_MODE_READ) {
    return f12_set_error(file->fs, F12_ERR_INVALID);
  }

  // Check disk
  f12_err_t err = f12_check_disk(file->fs);
  if (err != F12_OK) return err;

  // For seek, we need to re-open and skip bytes
  // This is inefficient but our fat12_file_t doesn't support seek directly
  // Reset reader to start
  fat12_err_t ferr = fat12_open(&file->fs->fat, &file->dirent, &file->reader);
  if (ferr != FAT12_OK) {
    return f12_set_error(file->fs, fat12_to_f12_err(ferr));
  }

  // Skip bytes up to offset
  uint8_t skip_buf[64];
  uint32_t to_skip = offset;
  while (to_skip > 0) {
    uint16_t chunk = (to_skip > sizeof(skip_buf)) ? sizeof(skip_buf) : to_skip;
    int n = fat12_read(&file->reader, skip_buf, chunk);
    if (n <= 0) break;
    to_skip -= n;
  }

  file->position = offset - to_skip;
  return F12_OK;
}

uint32_t f12_tell(f12_file_t *file) {
  if (!file) return 0;
  return file->position;
}

// ============== Random Access ==============

int f12_read_at(f12_file_t *file, uint32_t offset, void *buf, size_t len) {
  if (!file || !file->fs) return -1;

  // Save current position
  uint32_t saved_pos = file->position;

  // Seek to offset
  f12_err_t err = f12_seek(file, offset);
  if (err != F12_OK) return -1;

  // Read
  int n = f12_read(file, buf, len);

  // Restore position (best effort)
  f12_seek(file, saved_pos);

  return n;
}

int f12_write_at(f12_file_t *file, uint32_t offset, const void *buf, size_t len) {
  (void)offset;
  (void)buf;
  (void)len;

  // Random write not supported with current fat12 writer
  // It only supports sequential append
  if (!file || !file->fs) return -1;

  f12_set_error(file->fs, F12_ERR_INVALID);
  return -1;
}

// ============== File Management ==============

f12_err_t f12_stat(f12_t *fs, const char *path, f12_stat_t *stat) {
  if (!fs || !path || !stat) return F12_ERR_INVALID;

  f12_err_t err = f12_check_disk(fs);
  if (err != F12_OK) return err;

  // Skip leading slash
  if (path[0] == '/') path++;

  fat12_dirent_t entry;
  fat12_err_t ferr = fat12_find(&fs->fat, path, &entry);
  if (ferr != FAT12_OK) {
    return f12_set_error(fs, fat12_to_f12_err(ferr));
  }

  format_name_83(&entry, stat->name);
  stat->size = entry.size;
  stat->attr = entry.attr;
  stat->is_dir = (entry.attr & FAT12_ATTR_DIRECTORY) != 0;

  return F12_OK;
}

f12_err_t f12_delete(f12_t *fs, const char *path) {
  if (!fs || !path) return F12_ERR_INVALID;

  f12_err_t err = f12_check_writable(fs);
  if (err != F12_OK) return err;

  // Skip leading slash
  if (path[0] == '/') path++;

  fat12_err_t ferr = fat12_delete(&fs->fat, path);
  if (ferr != FAT12_OK) {
    return f12_set_error(fs, fat12_to_f12_err(ferr));
  }

  return F12_OK;
}

// ============== Directory Operations ==============

f12_err_t f12_opendir(f12_t *fs, const char *path, f12_dir_t *dir) {
  if (!fs || !path || !dir) return F12_ERR_INVALID;

  f12_err_t err = f12_check_disk(fs);
  if (err != F12_OK) return err;

  // Only root directory supported for now
  if (path[0] == '/') path++;
  if (path[0] != '\0') {
    return f12_set_error(fs, F12_ERR_NOT_FOUND);
  }

  dir->fs = fs;
  dir->index = 0;
  return F12_OK;
}

f12_err_t f12_readdir(f12_dir_t *dir, f12_stat_t *stat) {
  if (!dir || !dir->fs || !stat) return F12_ERR_INVALID;

  f12_err_t err = f12_check_disk(dir->fs);
  if (err != F12_OK) return err;

  // Iterate until we find a valid entry or reach end
  while (1) {
    fat12_dirent_t entry;
    fat12_err_t ferr = fat12_read_root_entry(&dir->fs->fat, dir->index, &entry);

    if (ferr != FAT12_OK) {
      return f12_set_error(dir->fs, F12_ERR_EOF);
    }

    dir->index++;

    // End of directory
    if (fat12_entry_is_end(&entry)) {
      return f12_set_error(dir->fs, F12_ERR_EOF);
    }

    // Skip invalid entries (deleted, LFN, volume label)
    if (!fat12_entry_valid(&entry)) {
      continue;
    }

    // Skip volume label
    if (entry.attr & FAT12_ATTR_VOLUME_ID) {
      continue;
    }

    // Found valid entry
    format_name_83(&entry, stat->name);
    stat->size = entry.size;
    stat->attr = entry.attr;
    stat->is_dir = (entry.attr & FAT12_ATTR_DIRECTORY) != 0;

    return F12_OK;
  }
}

f12_err_t f12_closedir(f12_dir_t *dir) {
  if (!dir) return F12_ERR_INVALID;
  dir->fs = NULL;
  dir->index = 0;
  return F12_OK;
}

f12_err_t f12_list(f12_t *fs, f12_list_cb cb, void *ctx) {
  if (!fs || !cb) return F12_ERR_INVALID;

  f12_dir_t dir;
  f12_err_t err = f12_opendir(fs, "/", &dir);
  if (err != F12_OK) return err;

  f12_stat_t stat;
  while (f12_readdir(&dir, &stat) == F12_OK) {
    cb(&stat, ctx);
  }

  f12_closedir(&dir);
  return F12_OK;
}

// ============== Error Handling ==============

f12_err_t f12_errno(f12_t *fs) {
  if (!fs) return F12_ERR_INVALID;
  return fs->last_error;
}

const char *f12_strerror(f12_err_t err) {
  switch (err) {
    case F12_OK:                 return "Success";
    case F12_ERR_IO:             return "I/O error";
    case F12_ERR_NOT_FOUND:      return "File not found";
    case F12_ERR_EXISTS:         return "File exists";
    case F12_ERR_FULL:           return "Disk full";
    case F12_ERR_TOO_MANY:       return "Too many open files";
    case F12_ERR_INVALID:        return "Invalid argument";
    case F12_ERR_IS_DIR:         return "Is a directory";
    case F12_ERR_NOT_MOUNTED:    return "Not mounted";
    case F12_ERR_EOF:            return "End of file";
    case F12_ERR_DISK_CHANGED:   return "Disk changed";
    case F12_ERR_WRITE_PROTECTED: return "Write protected";
    case F12_ERR_BAD_HANDLE:     return "Bad file handle";
    default:                     return "Unknown error";
  }
}
