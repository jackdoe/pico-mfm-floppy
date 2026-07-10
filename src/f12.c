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

static f12_result_t f12_result(f12_err_t error, size_t count) {
  return (f12_result_t){.error = error, .count = count};
}

static f12_err_t f12_from_block(block_status_t status) {
  switch (status) {
    case BLOCK_OK: return F12_OK;
    case BLOCK_ERR_INVALID: return F12_ERR_INVALID;
    case BLOCK_ERR_BUSY: return F12_ERR_BUSY;
    case BLOCK_ERR_TIMEOUT: return F12_ERR_TIMEOUT;
    case BLOCK_ERR_CRC: return F12_ERR_CRC;
    case BLOCK_ERR_WRONG_TRACK: return F12_ERR_WRONG_TRACK;
    case BLOCK_ERR_WRONG_SIDE: return F12_ERR_WRONG_SIDE;
    case BLOCK_ERR_NO_TRACK0: return F12_ERR_NO_TRACK0;
    case BLOCK_ERR_MEDIA_CHANGED: return F12_ERR_MEDIA_CHANGED;
    case BLOCK_ERR_WRITE_PROTECTED: return F12_ERR_WRITE_PROTECTED;
    case BLOCK_ERR_UNDERRUN: return F12_ERR_UNDERRUN;
    case BLOCK_ERR_OVERRUN: return F12_ERR_OVERRUN;
    case BLOCK_ERR_VERIFY: return F12_ERR_VERIFY;
    case BLOCK_ERR_CORRUPT: return F12_ERR_CORRUPT;
    case BLOCK_ERR_IO: return F12_ERR_IO;
  }
  return F12_ERR_IO;
}

static f12_err_t f12_from_fat(const f12_t *fs, fat12_err_t error) {
  switch (error) {
    case FAT12_OK: return F12_OK;
    case FAT12_ERR_READ:
    case FAT12_ERR_WRITE: {
      block_status_t status = fat12_last_io(&fs->fat);
      return status == BLOCK_OK ? F12_ERR_IO : f12_from_block(status);
    }
    case FAT12_ERR_INVALID: return F12_ERR_INVALID;
    case FAT12_ERR_NOT_FOUND: return F12_ERR_NOT_FOUND;
    case FAT12_ERR_EOF: return F12_END;
    case FAT12_ERR_FULL: return F12_ERR_FULL;
    case FAT12_ERR_EXISTS: return F12_ERR_EXISTS;
    case FAT12_ERR_READ_ONLY: return F12_ERR_READ_ONLY;
    case FAT12_ERR_BUSY: return F12_ERR_BUSY;
    case FAT12_ERR_AMBIGUOUS: return F12_ERR_AMBIGUOUS;
    case FAT12_ERR_CORRUPT: return F12_ERR_CORRUPT;
  }
  return F12_ERR_IO;
}

static void f12_cache_clear(f12_t *fs) {
  memset(fs->cache, 0, sizeof(fs->cache));
  fs->cache_clock = 0;
}

static uint64_t f12_cache_used(f12_t *fs) {
  fs->cache_clock++;
  if (fs->cache_clock != 0) return fs->cache_clock;
  for (size_t i = 0; i < F12_CACHE_TRACKS; i++) fs->cache[i].used = 0;
  fs->cache_clock = 1;
  return fs->cache_clock;
}

static track_t *f12_cache_find(f12_t *fs, uint8_t cylinder, uint8_t head) {
  for (size_t i = 0; i < F12_CACHE_TRACKS; i++) {
    f12_cache_entry_t *entry = &fs->cache[i];
    if (entry->occupied &&
        entry->track.cylinder == cylinder && entry->track.head == head) {
      entry->used = f12_cache_used(fs);
      return &entry->track;
    }
  }
  return NULL;
}

static block_status_t f12_cache_put(f12_t *fs, const track_t *track,
                                    bool authoritative, track_t **cached) {
  size_t target = F12_CACHE_TRACKS;
  size_t available = F12_CACHE_TRACKS;
  uint64_t oldest = UINT64_MAX;
  for (size_t i = 0; i < F12_CACHE_TRACKS; i++) {
    f12_cache_entry_t *entry = &fs->cache[i];
    if (entry->occupied &&
        entry->track.cylinder == track->cylinder &&
        entry->track.head == track->head) {
      target = i;
      break;
    }
    if (!entry->occupied) {
      if (available == F12_CACHE_TRACKS) available = i;
    } else if (entry->used < oldest) {
      oldest = entry->used;
      target = i;
    }
  }
  if (available != F12_CACHE_TRACKS &&
      (target == F12_CACHE_TRACKS ||
       fs->cache[target].track.cylinder != track->cylinder ||
       fs->cache[target].track.head != track->head)) {
    target = available;
  }
  f12_cache_entry_t *entry = &fs->cache[target];
  if (!entry->occupied || entry->track.cylinder != track->cylinder ||
      entry->track.head != track->head || authoritative) {
    entry->track = *track;
    entry->conflicted = 0;
  } else {
    block_status_t status = BLOCK_OK;
    for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
      uint32_t bit = 1u << sector;
      if (!track_has(track, sector)) continue;
      if ((entry->conflicted & bit) != 0) {
        status = BLOCK_ERR_CORRUPT;
      } else if (track_has(&entry->track, sector) &&
                 memcmp(entry->track.data[sector], track->data[sector],
                        DISK_SECTOR_SIZE) != 0) {
        entry->track.valid &= ~bit;
        entry->conflicted |= bit;
        status = BLOCK_ERR_CORRUPT;
      } else if (!track_has(&entry->track, sector)) {
        memcpy(entry->track.data[sector], track->data[sector],
               DISK_SECTOR_SIZE);
        track_mark(&entry->track, sector);
      }
    }
    entry->track.valid &= ~entry->conflicted;
    entry->occupied = true;
    entry->used = f12_cache_used(fs);
    if (cached) *cached = &entry->track;
    return status;
  }
  entry->occupied = true;
  entry->used = f12_cache_used(fs);
  if (cached) *cached = &entry->track;
  return BLOCK_OK;
}

static void f12_cache_evict(f12_t *fs, uint8_t cylinder, uint8_t head) {
  for (size_t i = 0; i < F12_CACHE_TRACKS; i++) {
    f12_cache_entry_t *entry = &fs->cache[i];
    if (entry->occupied && entry->track.cylinder == cylinder &&
        entry->track.head == head) {
      memset(entry, 0, sizeof(*entry));
      return;
    }
  }
}

static bool f12_track_complete(const track_t *track, uint8_t cylinder,
                               uint8_t head) {
  return track && track->cylinder == cylinder && track->head == head &&
         track->valid == DISK_TRACK_VALID;
}

static block_status_t f12_device_generation(f12_t *fs, uint32_t *generation) {
  if (!fs || !generation || !fs->device.media_generation) {
    return BLOCK_ERR_INVALID;
  }
  return fs->device.media_generation(fs->device.ctx, generation);
}

static block_status_t f12_block_media(f12_t *fs) {
  if ((fs->state != F12_STATE_MOUNTED &&
       fs->state != F12_STATE_BLOCK_ACCESS)) {
    return BLOCK_ERR_INVALID;
  }
  uint32_t generation;
  block_status_t status = f12_device_generation(fs, &generation);
  if (status != BLOCK_OK) {
    if (status == BLOCK_ERR_MEDIA_CHANGED) f12_cache_clear(fs);
    return status;
  }
  if (generation != fs->media_generation) {
    f12_cache_clear(fs);
    return BLOCK_ERR_MEDIA_CHANGED;
  }
  return BLOCK_OK;
}

static block_status_t f12_block_writable(f12_t *fs) {
  if (!fs || !fs->device.write_protected) return BLOCK_ERR_INVALID;
  if (!fs->device.write_track) return BLOCK_ERR_WRITE_PROTECTED;
  bool write_protected;
  block_status_t status =
      fs->device.write_protected(fs->device.ctx, &write_protected);
  if (status != BLOCK_OK) return status;
  return write_protected ? BLOCK_ERR_WRITE_PROTECTED : BLOCK_OK;
}

static block_status_t f12_refresh_track(f12_t *fs, uint8_t cylinder,
                                        uint8_t head, track_t **out) {
  block_status_t status = f12_block_media(fs);
  if (status != BLOCK_OK) return status;
  if (!disk_ch_valid(cylinder, head) || !out) return BLOCK_ERR_INVALID;

  memset(&fs->track_work, 0, sizeof(fs->track_work));
  fs->track_work.cylinder = cylinder;
  fs->track_work.head = head;
  status = fs->device.read_track(fs->device.ctx, fs->media_generation,
                                 cylinder, head, &fs->track_work);
  block_status_t media_status = f12_block_media(fs);
  if (media_status != BLOCK_OK) return media_status;
  if (fs->track_work.cylinder != cylinder || fs->track_work.head != head ||
      (fs->track_work.valid & ~DISK_TRACK_VALID) != 0) {
    return status == BLOCK_OK ? BLOCK_ERR_CORRUPT : status;
  }
  bool authoritative = status == BLOCK_OK &&
                       fs->track_work.valid == DISK_TRACK_VALID;
  track_t *cached = NULL;
  block_status_t cache_status =
      f12_cache_put(fs, &fs->track_work, authoritative, &cached);
  *out = cached;
  if (!*out) return BLOCK_ERR_IO;
  if (cache_status != BLOCK_OK) return cache_status;
  return status;
}

static block_status_t f12_load_sector(f12_t *fs, uint8_t cylinder,
                                      uint8_t head, uint8_t sector,
                                      track_t **out) {
  block_status_t status = f12_block_media(fs);
  if (status != BLOCK_OK) return status;
  if (!disk_ch_valid(cylinder, head) || !disk_sector_valid(sector) || !out) {
    return BLOCK_ERR_INVALID;
  }
  track_t *track = f12_cache_find(fs, cylinder, head);
  if (track && track_has(track, sector)) {
    *out = track;
    return BLOCK_OK;
  }
  status = f12_refresh_track(fs, cylinder, head, &track);
  if (track && track_has(track, sector)) {
    *out = track;
    return BLOCK_OK;
  }
  return status == BLOCK_OK ? BLOCK_ERR_CORRUPT : status;
}

static block_status_t f12_load_complete_track(f12_t *fs, uint8_t cylinder,
                                              uint8_t head, track_t **out) {
  block_status_t status = f12_block_media(fs);
  if (status != BLOCK_OK) return status;
  if (!disk_ch_valid(cylinder, head) || !out) return BLOCK_ERR_INVALID;
  track_t *track = f12_cache_find(fs, cylinder, head);
  if (f12_track_complete(track, cylinder, head)) {
    *out = track;
    return BLOCK_OK;
  }
  status = f12_refresh_track(fs, cylinder, head, &track);
  if (f12_track_complete(track, cylinder, head)) {
    *out = track;
    return BLOCK_OK;
  }
  return status == BLOCK_OK ? BLOCK_ERR_CORRUPT : status;
}

static block_status_t f12_block_read(void *ctx, uint16_t lba,
                                     uint8_t out[DISK_SECTOR_SIZE]) {
  f12_t *fs = (f12_t *)ctx;
  if (!fs || !out || lba >= DISK_SECTOR_COUNT) return BLOCK_ERR_INVALID;

  uint8_t cylinder;
  uint8_t head;
  uint8_t sector;
  if (!disk_lba_to_chs(lba, &cylinder, &head, &sector)) {
    return BLOCK_ERR_INVALID;
  }
  track_t *track;
  block_status_t status = f12_load_sector(fs, cylinder, head, sector, &track);
  if (status != BLOCK_OK) return status;
  memcpy(out, track->data[sector], DISK_SECTOR_SIZE);
  return f12_block_media(fs);
}

static block_status_t f12_block_write(void *ctx, const track_t *partial) {
  f12_t *fs = (f12_t *)ctx;
  if (!fs || !partial || !disk_ch_valid(partial->cylinder, partial->head) ||
      partial->valid == 0 || (partial->valid & ~DISK_TRACK_VALID) != 0) {
    return BLOCK_ERR_INVALID;
  }

  block_status_t status = f12_block_media(fs);
  if (status != BLOCK_OK) return status;
  status = f12_block_writable(fs);
  if (status != BLOCK_OK) return status;

  track_t *full = &fs->track_work;
  if (partial->valid == DISK_TRACK_VALID) {
    *full = *partial;
  } else {
    track_t *base;
    status = f12_load_complete_track(fs, partial->cylinder, partial->head,
                                     &base);
    if (status != BLOCK_OK) return status;
    *full = *base;
    for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
      if ((partial->valid & (1u << sector)) != 0) {
        memcpy(full->data[sector], partial->data[sector], DISK_SECTOR_SIZE);
      }
    }
    full->valid = DISK_TRACK_VALID;
  }

  status = f12_block_media(fs);
  if (status != BLOCK_OK) return status;

  status = fs->device.write_track(fs->device.ctx, fs->media_generation, full);
  if (status != BLOCK_OK) {
    if (status == BLOCK_ERR_MEDIA_CHANGED) f12_cache_clear(fs);
    else f12_cache_evict(fs, partial->cylinder, partial->head);
    return status;
  }
  status = f12_block_media(fs);
  if (status != BLOCK_OK) return status;
  return f12_cache_put(fs, full, true, NULL);
}

static fat12_io_t f12_fat_io(f12_t *fs) {
  return (fat12_io_t){
      .read = f12_block_read,
      .write = f12_block_write,
      .ctx = fs,
  };
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
  f12_cache_clear(fs);
  fs->state = F12_STATE_READY;
  fs->mount_generation = f12_next_generation(fs->mount_generation);
}

static f12_err_t f12_finish(f12_t *fs, f12_err_t error) {
  if (error == F12_ERR_MEDIA_CHANGED && f12_valid(fs) &&
      fs->state == F12_STATE_MOUNTED) {
    f12_invalidate(fs);
  }
  return error;
}

static f12_err_t f12_check_mounted(f12_t *fs) {
  if (!f12_valid(fs)) return F12_ERR_NOT_INITIALIZED;
  if (fs->state != F12_STATE_MOUNTED) return F12_ERR_NOT_MOUNTED;
  return f12_finish(fs, f12_from_block(f12_block_media(fs)));
}

static f12_err_t f12_check_writable(f12_t *fs) {
  f12_err_t error = f12_check_mounted(fs);
  if (error != F12_OK) return error;
  return f12_finish(fs, f12_from_block(f12_block_writable(fs)));
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

static bool f12_name_equal(const fat12_name_t *left, const fat12_name_t *right) {
  return memcmp(left, right, sizeof(*left)) == 0;
}

static bool f12_name_open(const f12_t *fs, const fat12_name_t *name) {
  for (size_t i = 0; i < F12_MAX_OPEN_FILES; i++) {
    if (fs->files[i].active && f12_name_equal(&fs->files[i].name, name)) return true;
  }
  return false;
}

static f12_file_slot_t *f12_file_slot(f12_file_t *file) {
  if (!file || !f12_valid(file->token.fs) ||
      file->token.fs->state != F12_STATE_MOUNTED ||
      file->token.slot >= F12_MAX_OPEN_FILES ||
      file->token.incarnation != file->token.fs->incarnation ||
      file->token.mount_generation != file->token.fs->mount_generation) {
    return NULL;
  }
  f12_file_slot_t *slot = &file->token.fs->files[file->token.slot];
  if (!slot->active || slot->generation != file->token.slot_generation) {
    return NULL;
  }
  return slot;
}

static const f12_file_slot_t *f12_file_slot_const(const f12_file_t *file) {
  return f12_file_slot((f12_file_t *)file);
}

static f12_dir_slot_t *f12_dir_slot(f12_dir_t *dir) {
  if (!dir || !f12_valid(dir->token.fs) ||
      dir->token.fs->state != F12_STATE_MOUNTED ||
      dir->token.slot >= F12_MAX_OPEN_DIRS ||
      dir->token.incarnation != dir->token.fs->incarnation ||
      dir->token.mount_generation != dir->token.fs->mount_generation) {
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

static void f12_format_name(const fat12_dirent_t *entry, char out[13]) {
  size_t pos = 0;
  size_t i = 0;
  while (i < FAT12_FILENAME_LEN && entry->name[i] != ' ') out[pos++] = entry->name[i++];
  if (entry->ext[0] != ' ') {
    out[pos++] = '.';
    i = 0;
    while (i < FAT12_EXTENSION_LEN && entry->ext[i] != ' ') out[pos++] = entry->ext[i++];
  }
  out[pos] = '\0';
}

static void f12_fill_stat(const fat12_dirent_t *entry, f12_stat_t *stat) {
  f12_format_name(entry, stat->name);
  stat->size = entry->size;
  stat->attr = entry->attr;
}

f12_err_t f12_init(f12_t *fs, block_device_t device) {
  if (!fs || !device.read_track || !device.media_generation ||
      !device.write_protected) {
    return F12_ERR_INVALID;
  }
  uint64_t incarnation = f12_next_incarnation();
  memset(fs, 0, sizeof(*fs));
  fs->device = device;
  fs->incarnation = incarnation;
  fs->signature = F12_SIGNATURE;
  fs->signature_inverse = ~F12_SIGNATURE;
  fs->mount_generation = 1;
  for (size_t i = 0; i < F12_MAX_OPEN_FILES; i++) fs->files[i].generation = 1;
  for (size_t i = 0; i < F12_MAX_OPEN_DIRS; i++) fs->dirs[i].generation = 1;
  return F12_OK;
}

f12_err_t f12_mount(f12_t *fs) {
  if (!f12_valid(fs)) return F12_ERR_NOT_INITIALIZED;
  if (fs->state == F12_STATE_MOUNTED) return F12_ERR_ALREADY_MOUNTED;
  if (fs->state != F12_STATE_READY) return F12_ERR_BUSY;
  if (f12_operations_open(fs) || fat12_busy(&fs->fat)) return F12_ERR_CONFLICT;

  f12_cache_clear(fs);
  block_status_t status =
      f12_device_generation(fs, &fs->media_generation);
  if (status != BLOCK_OK) return f12_from_block(status);
  fs->state = F12_STATE_BLOCK_ACCESS;
  fat12_err_t fat_error = fat12_init(&fs->fat, f12_fat_io(fs));
  if (fat_error != FAT12_OK) {
    fs->state = F12_STATE_READY;
    f12_cache_clear(fs);
    return f12_from_fat(fs, fat_error);
  }
  status = f12_block_media(fs);
  fs->state = F12_STATE_READY;
  if (status != BLOCK_OK) {
    f12_cache_clear(fs);
    return f12_from_block(status);
  }
  fs->mount_generation = f12_next_generation(fs->mount_generation);
  fs->state = F12_STATE_MOUNTED;
  return F12_OK;
}

f12_err_t f12_unmount(f12_t *fs) {
  f12_err_t error = f12_check_mounted(fs);
  if (error != F12_OK) return error;

  if (f12_operations_open(fs)) return F12_ERR_CONFLICT;
  if (fat12_busy(&fs->fat)) return F12_ERR_BUSY;
  f12_cache_clear(fs);
  fs->state = F12_STATE_READY;
  fs->mount_generation = f12_next_generation(fs->mount_generation);
  return F12_OK;
}

f12_err_t f12_format(f12_t *fs, f12_format_options_t options) {
  if (!f12_valid(fs)) return F12_ERR_NOT_INITIALIZED;
  if (!options.label ||
      (options.mode != F12_FORMAT_QUICK && options.mode != F12_FORMAT_FULL)) {
    return F12_ERR_INVALID;
  }
  if (fs->state == F12_STATE_MOUNTED) return F12_ERR_ALREADY_MOUNTED;
  if (fs->state != F12_STATE_READY) return F12_ERR_BUSY;
  if (f12_operations_open(fs) || fat12_busy(&fs->fat)) return F12_ERR_CONFLICT;
  block_status_t status = f12_block_writable(fs);
  if (status != BLOCK_OK) return f12_from_block(status);

  f12_cache_clear(fs);
  status = f12_device_generation(fs, &fs->media_generation);
  if (status != BLOCK_OK) return f12_from_block(status);
  fs->state = F12_STATE_BLOCK_ACCESS;
  fat12_err_t fat_error = fat12_format(&fs->fat, f12_fat_io(fs), options.label,
                                       options.mode == F12_FORMAT_FULL,
                                       options.progress, options.progress_ctx);
  if (fat_error != FAT12_OK) {
    fs->state = F12_STATE_READY;
    return f12_from_fat(fs, fat_error);
  }
  status = f12_block_media(fs);
  fs->state = F12_STATE_READY;
  return f12_from_block(status);
}

f12_err_t f12_fsck(f12_t *fs, fat12_fsck_t *report, bool repair) {
  if (!report) return F12_ERR_INVALID;
  f12_err_t error = repair ? f12_check_writable(fs) : f12_check_mounted(fs);
  if (error != F12_OK) return error;
  if ((repair && f12_operations_open(fs)) || f12_writer_open(fs) || fat12_busy(&fs->fat)) {
    return F12_ERR_CONFLICT;
  }
  fat12_err_t fat_error = fat12_fsck(&fs->fat, report, repair);
  return f12_finish(fs, f12_from_fat(fs, fat_error));
}

f12_err_t f12_is_mounted(f12_t *fs, bool *mounted) {
  if (mounted) *mounted = false;
  if (!mounted) return F12_ERR_INVALID;
  if (!f12_valid(fs)) return F12_ERR_NOT_INITIALIZED;
  if (fs->state == F12_STATE_READY) return F12_OK;
  if (fs->state != F12_STATE_MOUNTED) return F12_ERR_BUSY;
  f12_err_t error = f12_check_mounted(fs);
  if (error == F12_OK) *mounted = true;
  return error;
}

f12_err_t f12_open(f12_t *fs, const char *path, f12_open_mode_t mode,
                   f12_file_t *out) {
  if (out) memset(out, 0, sizeof(*out));
  if (!out || !path || (mode != F12_OPEN_READ && mode != F12_OPEN_WRITE)) {
    return F12_ERR_INVALID;
  }

  f12_err_t error = mode == F12_OPEN_WRITE ? f12_check_writable(fs) : f12_check_mounted(fs);
  if (error != F12_OK) return error;
  const char *name = f12_path(path);
  fat12_name_t parsed;
  fat12_err_t fat_error = fat12_name_parse(name, &parsed);
  if (fat_error != FAT12_OK) return f12_from_fat(fs, fat_error);
  if ((mode == F12_OPEN_READ && f12_writer_open(fs) && f12_name_open(fs, &parsed)) ||
      (mode == F12_OPEN_WRITE &&
       (f12_writer_open(fs) || fat12_busy(&fs->fat) || f12_name_open(fs, &parsed)))) {
    return F12_ERR_CONFLICT;
  }

  size_t index = F12_MAX_OPEN_FILES;
  for (size_t i = 0; i < F12_MAX_OPEN_FILES; i++) {
    if (!fs->files[i].active) {
      index = i;
      break;
    }
  }
  if (index == F12_MAX_OPEN_FILES) return F12_ERR_TOO_MANY;

  f12_file_slot_t *slot = &fs->files[index];
  if (mode == F12_OPEN_READ) {
    fat12_dirent_t entry;
    fat_error = fat12_find(&fs->fat, name, &entry);
    if (fat_error != FAT12_OK) return f12_finish(fs, f12_from_fat(fs, fat_error));
    if ((entry.attr & FAT12_ATTR_DIRECTORY) != 0) return F12_ERR_IS_DIR;
    fat_error = fat12_open(&fs->fat, &entry, &slot->io.reader);
    if (fat_error != FAT12_OK) {
      return f12_finish(fs, f12_from_fat(fs, fat_error));
    }
  } else {
    fat_error = fat12_open_write(&fs->fat, name, &slot->io.writer);
    if (fat_error != FAT12_OK) return f12_finish(fs, f12_from_fat(fs, fat_error));
  }

  slot->generation = f12_next_generation(slot->generation);
  slot->name = parsed;
  slot->mode = mode;
  slot->active = true;
  *out = (f12_file_t){.token = {
      .fs = fs,
      .incarnation = fs->incarnation,
      .mount_generation = fs->mount_generation,
      .slot_generation = slot->generation,
      .slot = (uint16_t)index,
  },
  };
  return F12_OK;
}

f12_err_t f12_close(f12_file_t *file) {
  f12_file_slot_t *slot = f12_file_slot(file);
  if (!slot) return F12_ERR_BAD_HANDLE;
  f12_t *fs = file->token.fs;
  f12_err_t error = f12_check_mounted(fs);
  if (error != F12_OK) return error;

  if (slot->mode == F12_OPEN_WRITE) {
    fat12_err_t fat_error = fat12_close_write(&slot->io.writer);
    if (fat_error != FAT12_OK) return f12_finish(fs, f12_from_fat(fs, fat_error));
  }
  f12_release_file_slot(slot);
  memset(file, 0, sizeof(*file));
  return F12_OK;
}

f12_err_t f12_abort(f12_file_t *file) {
  f12_file_slot_t *slot = f12_file_slot(file);
  if (!slot) return F12_ERR_BAD_HANDLE;
  if (slot->mode != F12_OPEN_WRITE) return F12_ERR_CONFLICT;
  f12_t *fs = file->token.fs;
  f12_err_t error = f12_check_mounted(fs);
  if (error != F12_OK) return error;
  fat12_err_t fat_error = fat12_abort_write(&slot->io.writer);
  if (fat_error != FAT12_OK) return f12_from_fat(fs, fat_error);
  f12_release_file_slot(slot);
  memset(file, 0, sizeof(*file));
  return F12_OK;
}

f12_result_t f12_read(f12_file_t *file, void *buf, size_t len) {
  f12_file_slot_t *slot = f12_file_slot(file);
  if (!slot) return f12_result(F12_ERR_BAD_HANDLE, 0);
  if (slot->mode != F12_OPEN_READ) return f12_result(F12_ERR_CONFLICT, 0);
  if (!buf && len != 0) return f12_result(F12_ERR_INVALID, 0);
  if (len == 0) return f12_result(F12_OK, 0);

  f12_err_t error = f12_check_mounted(file->token.fs);
  if (error != F12_OK) return f12_result(error, 0);
  fat12_result_t result = fat12_read(&slot->io.reader, (uint8_t *)buf, len);
  error = f12_finish(file->token.fs,
                     f12_from_fat(file->token.fs, result.error));
  if (error != F12_OK) return f12_result(error, result.count);
  return f12_result(result.count == 0 ? F12_END : F12_OK, result.count);
}

f12_result_t f12_write(f12_file_t *file, const void *buf, size_t len) {
  f12_file_slot_t *slot = f12_file_slot(file);
  if (!slot) return f12_result(F12_ERR_BAD_HANDLE, 0);
  if (slot->mode != F12_OPEN_WRITE) return f12_result(F12_ERR_CONFLICT, 0);
  if (!buf && len != 0) return f12_result(F12_ERR_INVALID, 0);
  if (len == 0) return f12_result(F12_OK, 0);

  f12_err_t error = f12_check_writable(file->token.fs);
  if (error != F12_OK) return f12_result(error, 0);
  fat12_result_t result = fat12_write(&slot->io.writer, (const uint8_t *)buf, len);
  error = f12_finish(file->token.fs,
                     f12_from_fat(file->token.fs, result.error));
  if (error != F12_OK) return f12_result(error, result.count);
  if (result.count == 0) return f12_result(F12_ERR_IO, 0);
  return f12_result(F12_OK, result.count);
}

f12_err_t f12_seek(f12_file_t *file, uint32_t offset) {
  f12_file_slot_t *slot = f12_file_slot(file);
  if (!slot) return F12_ERR_BAD_HANDLE;
  if (slot->mode != F12_OPEN_READ) return F12_ERR_CONFLICT;
  f12_err_t error = f12_check_mounted(file->token.fs);
  if (error != F12_OK) return error;
  fat12_err_t fat_error = fat12_seek(&slot->io.reader, offset);
  return f12_finish(file->token.fs,
                    f12_from_fat(file->token.fs, fat_error));
}

f12_err_t f12_tell(const f12_file_t *file, uint32_t *offset) {
  if (!offset) return F12_ERR_INVALID;
  const f12_file_slot_t *slot = f12_file_slot_const(file);
  if (!slot) return F12_ERR_BAD_HANDLE;
  f12_err_t error = f12_check_mounted(file->token.fs);
  if (error != F12_OK) return error;
  if (slot->mode == F12_OPEN_READ) {
    *offset = slot->io.reader.bytes_read;
  } else if (slot->mode == F12_OPEN_WRITE) {
    *offset = slot->io.writer.bytes_written;
  } else {
    return F12_ERR_BAD_HANDLE;
  }
  return F12_OK;
}

f12_result_t f12_read_at(f12_file_t *file, uint32_t offset, void *buf,
                         size_t len) {
  f12_file_slot_t *slot = f12_file_slot(file);
  if (!slot) return f12_result(F12_ERR_BAD_HANDLE, 0);
  if (slot->mode != F12_OPEN_READ) return f12_result(F12_ERR_CONFLICT, 0);
  if (!buf && len != 0) {
    return f12_result(F12_ERR_INVALID, 0);
  }
  f12_t *fs = file->token.fs;
  f12_err_t error = f12_check_mounted(fs);
  if (error != F12_OK) return f12_result(error, 0);
  fat12_file_t reader = slot->io.reader;
  fat12_err_t fat_error = fat12_seek(&reader, offset);
  if (fat_error != FAT12_OK) {
    return f12_result(f12_finish(fs, f12_from_fat(fs, fat_error)), 0);
  }
  if (len == 0) return f12_result(F12_OK, 0);
  fat12_result_t result = fat12_read(&reader, (uint8_t *)buf, len);
  error = f12_finish(fs, f12_from_fat(fs, result.error));
  if (error != F12_OK) return f12_result(error, result.count);
  return f12_result(result.count == 0 ? F12_END : F12_OK, result.count);
}

f12_err_t f12_stat(f12_t *fs, const char *path, f12_stat_t *stat) {
  if (stat) memset(stat, 0, sizeof(*stat));
  if (!path || !stat) return F12_ERR_INVALID;
  f12_err_t error = f12_check_mounted(fs);
  if (error != F12_OK) return error;
  fat12_dirent_t entry;
  fat12_err_t fat_error = fat12_find(&fs->fat, f12_path(path), &entry);
  if (fat_error != FAT12_OK) return f12_finish(fs, f12_from_fat(fs, fat_error));
  f12_fill_stat(&entry, stat);
  return F12_OK;
}

f12_err_t f12_free_count(f12_t *fs, uint16_t *count) {
  if (count) *count = 0;
  if (!count) return F12_ERR_INVALID;
  f12_err_t error = f12_check_mounted(fs);
  if (error != F12_OK) return error;
  if (f12_writer_open(fs) || fat12_busy(&fs->fat)) return F12_ERR_CONFLICT;
  fat12_err_t fat_error = fat12_free_count(&fs->fat, count);
  return f12_finish(fs, f12_from_fat(fs, fat_error));
}

f12_err_t f12_delete(f12_t *fs, const char *path) {
  if (!path) return F12_ERR_INVALID;
  f12_err_t error = f12_check_writable(fs);
  if (error != F12_OK) return error;
  if (f12_operations_open(fs) || fat12_busy(&fs->fat)) return F12_ERR_CONFLICT;
  fat12_err_t fat_error = fat12_delete(&fs->fat, f12_path(path));
  return f12_finish(fs, f12_from_fat(fs, fat_error));
}

f12_err_t f12_rename(f12_t *fs, const char *from, const char *to) {
  if (!from || !to) return F12_ERR_INVALID;
  f12_err_t error = f12_check_writable(fs);
  if (error != F12_OK) return error;
  if (f12_operations_open(fs) || fat12_busy(&fs->fat)) return F12_ERR_CONFLICT;
  fat12_err_t fat_error = fat12_rename(&fs->fat, f12_path(from), f12_path(to));
  return f12_finish(fs, f12_from_fat(fs, fat_error));
}

f12_err_t f12_opendir(f12_t *fs, const char *path, f12_dir_t *dir) {
  if (dir) memset(dir, 0, sizeof(*dir));
  if (!path || !dir) return F12_ERR_INVALID;
  f12_err_t error = f12_check_mounted(fs);
  if (error != F12_OK) return error;
  const char *name = f12_path(path);
  if (*name != '\0') return F12_ERR_NOT_DIR;
  if (f12_writer_open(fs) || fat12_busy(&fs->fat)) return F12_ERR_CONFLICT;

  size_t index = F12_MAX_OPEN_DIRS;
  for (size_t i = 0; i < F12_MAX_OPEN_DIRS; i++) {
    if (!fs->dirs[i].active) {
      index = i;
      break;
    }
  }
  if (index == F12_MAX_OPEN_DIRS) return F12_ERR_TOO_MANY;

  f12_dir_slot_t *slot = &fs->dirs[index];
  slot->generation = f12_next_generation(slot->generation);
  slot->index = 0;
  slot->active = true;
  *dir = (f12_dir_t){.token = {
      .fs = fs,
      .incarnation = fs->incarnation,
      .mount_generation = fs->mount_generation,
      .slot_generation = slot->generation,
      .slot = (uint16_t)index,
  },
  };
  return F12_OK;
}

f12_err_t f12_readdir(f12_dir_t *dir, f12_stat_t *stat) {
  if (stat) memset(stat, 0, sizeof(*stat));
  if (!stat) return F12_ERR_INVALID;
  f12_dir_slot_t *slot = f12_dir_slot(dir);
  if (!slot) return F12_ERR_BAD_HANDLE;
  f12_err_t error = f12_check_mounted(dir->token.fs);
  if (error != F12_OK) return error;

  for (;;) {
    fat12_dirent_t entry;
    fat12_err_t fat_error = fat12_read_root_entry(&dir->token.fs->fat,
                                                  slot->index, &entry);
    if (fat_error != FAT12_OK) {
      return f12_finish(dir->token.fs,
                        f12_from_fat(dir->token.fs, fat_error));
    }
    slot->index++;
    if (fat12_entry_is_end(&entry)) return F12_END;
    if (!fat12_entry_valid(&entry) || (entry.attr & FAT12_ATTR_VOLUME_ID) != 0) continue;
    f12_fill_stat(&entry, stat);
    return F12_OK;
  }
}

f12_err_t f12_closedir(f12_dir_t *dir) {
  f12_dir_slot_t *slot = f12_dir_slot(dir);
  if (!slot) return F12_ERR_BAD_HANDLE;
  f12_release_dir_slot(slot);
  memset(dir, 0, sizeof(*dir));
  return F12_OK;
}

f12_err_t f12_list(f12_t *fs, f12_list_fn fn, void *ctx) {
  if (!fn) return F12_ERR_INVALID;
  f12_dir_t dir;
  f12_err_t error = f12_opendir(fs, "/", &dir);
  if (error != F12_OK) return error;

  for (;;) {
    f12_stat_t stat;
    error = f12_readdir(&dir, &stat);
    if (error == F12_END) {
      return f12_closedir(&dir);
    }
    if (error != F12_OK) {
      f12_closedir(&dir);
      return error;
    }
    error = fn(ctx, &stat);
    if (error != F12_OK) {
      f12_closedir(&dir);
      return error;
    }
  }
}

const char *f12_strerror(f12_err_t error) {
  switch (error) {
    case F12_OK: return "success";
    case F12_END: return "end";
    case F12_ERR_INVALID: return "invalid argument";
    case F12_ERR_NOT_INITIALIZED: return "not initialized";
    case F12_ERR_NOT_MOUNTED: return "not mounted";
    case F12_ERR_ALREADY_MOUNTED: return "already mounted";
    case F12_ERR_NOT_FOUND: return "not found";
    case F12_ERR_EXISTS: return "already exists";
    case F12_ERR_FULL: return "disk full";
    case F12_ERR_TOO_MANY: return "too many open operations";
    case F12_ERR_IS_DIR: return "is a directory";
    case F12_ERR_NOT_DIR: return "not a directory";
    case F12_ERR_READ_ONLY: return "read only";
    case F12_ERR_CONFLICT: return "operation conflict";
    case F12_ERR_BUSY: return "busy";
    case F12_ERR_BAD_HANDLE: return "bad handle";
    case F12_ERR_TIMEOUT: return "timeout";
    case F12_ERR_CRC: return "CRC error";
    case F12_ERR_WRONG_TRACK: return "wrong track";
    case F12_ERR_WRONG_SIDE: return "wrong side";
    case F12_ERR_NO_TRACK0: return "track zero unavailable";
    case F12_ERR_MEDIA_CHANGED: return "media changed";
    case F12_ERR_WRITE_PROTECTED: return "write protected";
    case F12_ERR_UNDERRUN: return "underrun";
    case F12_ERR_OVERRUN: return "overrun";
    case F12_ERR_VERIFY: return "verification failed";
    case F12_ERR_AMBIGUOUS: return "ambiguous filesystem state";
    case F12_ERR_CORRUPT: return "corrupt filesystem";
    case F12_ERR_IO: return "I/O error";
  }
  return "unknown error";
}
