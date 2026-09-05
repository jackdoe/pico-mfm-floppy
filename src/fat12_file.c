#include "internal/fat12_internal.h"
#include <string.h>

bool fat12_busy(const fat12_t *fat) { return fat != NULL && fat->transaction; }

static disk_err_t fat12_mutable(fat12_t *fat) {
  disk_err_t err = cache_writable(fat->cache);
  if (err != DISK_OK) return err;
  return fat->transaction ? DISK_ERR_BUSY : DISK_OK;
}

void fat12_transaction_end(fat12_t *fat) {
  cache_discard(fat->cache);
  fat->transaction = false;
}

static disk_err_t fat12_read_cluster(fat12_t *fat, uint16_t cluster,
                                     uint8_t *buf) {
  if (!fat12_cluster_valid(cluster)) return DISK_ERR_INVALID;
  return cache_read(fat->cache, fat12_cluster_to_lba(cluster), buf);
}

static disk_err_t fat12_validate_file_entry(fat12_t *fat,
                                            const fat12_dirent_t *entry) {
  if ((entry->attr & (FAT12_ATTR_DIRECTORY | FAT12_ATTR_VOLUME_ID)) != 0) {
    return DISK_ERR_INVALID;
  }
  uint32_t clusters =
      entry->size / DISK_SECTOR_SIZE + (entry->size % DISK_SECTOR_SIZE != 0);
  if (clusters == 0) {
    return entry->start_cluster == 0 ? DISK_OK : DISK_ERR_CORRUPT;
  }
  if (clusters > FAT12_DATA_CLUSTERS ||
      !fat12_cluster_valid(entry->start_cluster)) {
    return DISK_ERR_CORRUPT;
  }
  uint16_t cluster = entry->start_cluster;
  for (uint32_t index = 0; index < clusters; index++) {
    if (!fat12_cluster_valid(cluster)) return DISK_ERR_CORRUPT;
    uint16_t next;
    disk_err_t err = fat12_get_entry(fat, cluster, &next);
    if (err != DISK_OK) return err;
    if (index + 1u == clusters) {
      return fat12_is_eof(next) ? DISK_OK : DISK_ERR_CORRUPT;
    }
    if (!fat12_cluster_valid(next) || fat12_is_bad(next)) {
      return DISK_ERR_CORRUPT;
    }
    cluster = next;
  }
  return DISK_ERR_CORRUPT;
}

disk_err_t fat12_open(fat12_t *fat, const fat12_dirent_t *entry,
                      fat12_file_t *file) {
  if (fat == NULL || entry == NULL || file == NULL) return DISK_ERR_INVALID;
  disk_err_t err = fat12_validate_file_entry(fat, entry);
  if (err != DISK_OK) return err;
  memset(file, 0, sizeof(*file));
  file->fat = fat;
  file->start_cluster = entry->start_cluster;
  file->current_cluster = entry->start_cluster;
  file->file_size = entry->size;
  return DISK_OK;
}

static disk_err_t fat12_chain_step(fat12_t *fat, uint16_t cluster,
                                   uint16_t *next) {
  if (!fat12_cluster_valid(cluster)) return DISK_ERR_CORRUPT;
  disk_err_t err = fat12_get_entry(fat, cluster, next);
  if (err != DISK_OK) return err;
  return fat12_is_eof(*next) || fat12_cluster_valid(*next) ? DISK_OK
                                                           : DISK_ERR_CORRUPT;
}

static disk_err_t fat12_chain_at(fat12_t *fat, uint16_t start, uint32_t index,
                                 uint16_t *out) {
  uint16_t current = start;
  uint16_t hare = start;
  bool cycle_check = fat12_cluster_valid(start);
  for (uint32_t step = 0; step < index; step++) {
    disk_err_t err = fat12_chain_step(fat, current, &current);
    if (err != DISK_OK) return err;
    if (cycle_check) {
      for (uint8_t hop = 0; hop < 2; hop++) {
        if (fat12_is_eof(hare)) {
          cycle_check = false;
          break;
        }
        err = fat12_chain_step(fat, hare, &hare);
        if (err != DISK_OK) return err;
      }
      if (cycle_check && current == hare) return DISK_ERR_CORRUPT;
    }
  }
  *out = current;
  return DISK_OK;
}

disk_err_t fat12_seek(fat12_file_t *file, uint32_t offset) {
  if (file == NULL || file->fat == NULL) return DISK_ERR_INVALID;
  fat12_t *fat = file->fat;
  if (offset > file->file_size) offset = file->file_size;
  uint32_t steps = offset / DISK_SECTOR_SIZE;
  if (steps > FAT12_DATA_CLUSTERS) return DISK_ERR_CORRUPT;
  uint16_t cluster;
  disk_err_t err = fat12_chain_at(fat, file->start_cluster, steps, &cluster);
  if (err != DISK_OK) return err;
  if (offset < file->file_size && !fat12_cluster_valid(cluster)) {
    return DISK_ERR_CORRUPT;
  }
  file->current_cluster = cluster;
  file->bytes_read = offset;
  file->buffer_valid = false;
  return DISK_OK;
}

static disk_err_t fat12_read_current_cluster(fat12_file_t *file) {
  if (!file->buffer_valid) {
    disk_err_t err =
        fat12_read_cluster(file->fat, file->current_cluster, file->cluster_buf);
    if (err != DISK_OK) return err;
    file->buffer_valid = true;
  }
  return DISK_OK;
}

disk_result_t fat12_read(fat12_file_t *file, uint8_t *buf, size_t len) {
  disk_result_t result = {.error = DISK_OK, .count = 0};
  if (file == NULL || file->fat == NULL || (buf == NULL && len != 0)) {
    result.error = DISK_ERR_INVALID;
    return result;
  }
  fat12_t *fat = file->fat;
  while (len > 0 && file->bytes_read < file->file_size) {
    if (!fat12_cluster_valid(file->current_cluster) ||
        file->bytes_read / DISK_SECTOR_SIZE >= FAT12_DATA_CLUSTERS) {
      result.error = DISK_ERR_CORRUPT;
      return result;
    }
    disk_err_t err = fat12_read_current_cluster(file);
    if (err != DISK_OK) {
      result.error = err;
      return result;
    }
    size_t offset = file->bytes_read % DISK_SECTOR_SIZE;
    size_t count = DISK_SECTOR_SIZE - offset;
    size_t remaining = file->file_size - file->bytes_read;
    if (count > remaining) count = remaining;
    if (count > len) count = len;
    bool advance = file->bytes_read + count < file->file_size &&
                   offset + count == DISK_SECTOR_SIZE;
    uint16_t next = 0;
    if (advance) {
      err = fat12_chain_step(fat, file->current_cluster, &next);
      if (err != DISK_OK) {
        result.error = err;
        return result;
      }
    }
    memcpy(buf, file->cluster_buf + offset, count);
    buf += count;
    len -= count;
    result.count += count;
    file->bytes_read += (uint32_t)count;

    if (advance) {
      file->current_cluster = next;
      file->buffer_valid = false;
    }
  }
  return result;
}

static disk_err_t fat12_write_cluster(fat12_t *fat, uint16_t cluster,
                                      const uint8_t *buf) {
  if (!fat12_cluster_valid(cluster)) return DISK_ERR_INVALID;
  return cache_write(fat->cache, fat12_cluster_to_lba(cluster), buf);
}

static void fat12_init_dirent(fat12_dirent_t *entry, const fat12_name_t *name) {
  memset(entry, 0, sizeof(*entry));
  memcpy(entry->name, name->name, sizeof(entry->name));
  memcpy(entry->ext, name->ext, sizeof(entry->ext));
  entry->attr = FAT12_ATTR_ARCHIVE;
}

static disk_err_t fat12_validate_chain(fat12_t *fat, uint16_t start) {
  if (!fat12_cluster_valid(start)) return DISK_ERR_CORRUPT;
  uint16_t cluster = start;
  for (uint16_t count = 0; count < FAT12_DATA_CLUSTERS; count++) {
    uint16_t next;
    disk_err_t err = fat12_get_entry(fat, cluster, &next);
    if (err != DISK_OK) return err;
    if (fat12_is_eof(next)) return DISK_OK;
    if (!fat12_cluster_valid(next) || fat12_is_bad(next)) {
      return DISK_ERR_CORRUPT;
    }
    cluster = next;
  }
  return DISK_ERR_CORRUPT;
}

static disk_err_t fat12_validate_entry_chain(fat12_t *fat,
                                             const fat12_dirent_t *entry) {
  if (entry->start_cluster == 0) {
    return entry->size == 0 ? DISK_OK : DISK_ERR_CORRUPT;
  }
  return fat12_validate_chain(fat, entry->start_cluster);
}

static disk_err_t fat12_require_exclusive_chains(fat12_t *fat) {
  fat12_fsck_t report;
  disk_err_t err = fat12_fsck(fat, &report, false);
  if (err != DISK_OK) return err;
  return report.crosslinked == 0 && report.loops == 0 &&
                 report.broken_chains == 0 && report.size_mismatches == 0 &&
                 report.duplicate_names == 0 && !report.fat_mismatch &&
                 !report.fat_markers_invalid
             ? DISK_OK
             : DISK_ERR_CORRUPT;
}

disk_err_t fat12_open_write(fat12_t *fat, const char *filename,
                            fat12_writer_t *writer) {
  if (fat == NULL || writer == NULL) return DISK_ERR_INVALID;
  memset(writer, 0, sizeof(*writer));
  disk_err_t err = fat12_mutable(fat);
  if (err != DISK_OK) return err;

  fat12_name_t name;
  err = fat12_name_parse(filename, &name);
  if (err != DISK_OK) return err;
  fat12_lookup_t hit;
  err = fat12_root_lookup(fat, &name, &hit);
  if (err != DISK_OK) return err;

  if (hit.found) {
    if ((hit.entry.attr & FAT12_ATTR_DIRECTORY) != 0) return DISK_ERR_IS_DIR;
    if ((hit.entry.attr & FAT12_ATTR_READ_ONLY) != 0) return DISK_ERR_READ_ONLY;
    err = fat12_validate_entry_chain(fat, &hit.entry);
    if (err != DISK_OK) return err;
    writer->dirent_index = hit.index;
    writer->dirent = hit.entry;
  } else {
    if (hit.free_index == UINT16_MAX) return DISK_ERR_FULL;
    writer->dirent_index = hit.free_index;
    writer->consumed_end = hit.free_is_end;
    fat12_init_dirent(&writer->dirent, &name);
  }
  err = fat12_require_exclusive_chains(fat);
  if (err != DISK_OK) return err;
  fat->transaction = true;
  writer->fat = fat;
  writer->phase = FAT12_WRITER_DATA;
  return DISK_OK;
}

static disk_err_t fat12_writer_alloc_cluster(fat12_writer_t *writer) {
  fat12_t *fat = writer->fat;
  disk_err_t err;
  if (writer->pending_cluster == 0) {
    uint16_t start = writer->prev_cluster + 1u;
    if (start < 2 || start >= FAT12_CLUSTER_LIMIT) start = 2;
    err = fat12_find_free_cluster(fat, start, &writer->pending_cluster);
    if (err != DISK_OK) return err;
  }
  uint16_t cluster = writer->pending_cluster;
  err = fat12_set_entry(fat, cluster, FAT12_EOC);
  if (err != DISK_OK) return err;
  if (writer->prev_cluster != 0) {
    err = fat12_set_entry(fat, writer->prev_cluster, cluster);
    if (err != DISK_OK) return err;
  }
  if (writer->first_cluster == 0) writer->first_cluster = cluster;
  writer->current_cluster = cluster;
  writer->pending_cluster = 0;
  memset(writer->cluster_buf, 0, sizeof(writer->cluster_buf));
  return DISK_OK;
}

static uint16_t fat12_writer_offset(const fat12_writer_t *writer) {
  uint16_t offset = writer->bytes_written % DISK_SECTOR_SIZE;
  return writer->current_cluster != 0 && writer->bytes_written != 0 &&
                 offset == 0
             ? DISK_SECTOR_SIZE
             : offset;
}

static disk_err_t fat12_writer_flush_cluster(fat12_writer_t *writer) {
  if (writer->current_cluster == 0) return DISK_OK;
  return fat12_write_cluster(writer->fat, writer->current_cluster,
                             writer->cluster_buf);
}

static disk_err_t fat12_writer_prepare_next_cluster(fat12_writer_t *writer) {
  disk_err_t err = fat12_writer_flush_cluster(writer);
  if (err != DISK_OK) return err;
  if (writer->current_cluster != 0) {
    writer->prev_cluster = writer->current_cluster;
    writer->current_cluster = 0;
  }
  return fat12_writer_alloc_cluster(writer);
}

disk_result_t fat12_write(fat12_writer_t *writer, const uint8_t *buf,
                          size_t len) {
  disk_result_t result = {.error = DISK_OK, .count = 0};
  if (writer == NULL || writer->fat == NULL || (buf == NULL && len != 0)) {
    result.error = DISK_ERR_INVALID;
    return result;
  }
  if (writer->phase != FAT12_WRITER_DATA) {
    result.error = DISK_ERR_BUSY;
    return result;
  }
  if (writer->error != DISK_OK) {
    result.error = writer->error;
    return result;
  }
  while (len > 0) {
    uint16_t offset = fat12_writer_offset(writer);
    if (writer->current_cluster == 0 || offset == DISK_SECTOR_SIZE) {
      disk_err_t err = fat12_writer_prepare_next_cluster(writer);
      if (err != DISK_OK) {
        result.error = err;
        if (!disk_err_is_io(err)) writer->error = err;
        return result;
      }
      offset = 0;
    }
    size_t count = DISK_SECTOR_SIZE - offset;
    if (count > len) count = len;
    memcpy(writer->cluster_buf + offset, buf, count);
    writer->bytes_written += (uint32_t)count;
    buf += count;
    len -= count;
    result.count += count;
  }
  return result;
}

static disk_err_t fat12_stage_free_chain(fat12_t *fat, uint16_t start) {
  uint16_t cluster = start;
  for (uint16_t count = 0; count < FAT12_DATA_CLUSTERS; count++) {
    if (!fat12_cluster_valid(cluster)) return DISK_ERR_CORRUPT;
    uint16_t next;
    disk_err_t err = fat12_get_entry(fat, cluster, &next);
    if (err != DISK_OK) return err;
    err = fat12_set_entry(fat, cluster, 0);
    if (err != DISK_OK) return err;
    if (fat12_is_eof(next)) return DISK_OK;
    if (!fat12_cluster_valid(next) || fat12_is_bad(next)) {
      return DISK_ERR_CORRUPT;
    }
    cluster = next;
  }
  return DISK_ERR_CORRUPT;
}

disk_err_t fat12_abort_write(fat12_writer_t *writer) {
  if (writer == NULL || writer->fat == NULL) return DISK_ERR_INVALID;
  if (writer->phase == FAT12_WRITER_DONE) return DISK_OK;
  if (writer->phase != FAT12_WRITER_DATA) return DISK_ERR_BUSY;
  fat12_transaction_end(writer->fat);
  writer->phase = FAT12_WRITER_DONE;
  return DISK_OK;
}

void fat12_forget_write(fat12_writer_t *writer) {
  if (writer == NULL) return;
  if (writer->fat != NULL) fat12_transaction_end(writer->fat);
  writer->fat = NULL;
  writer->phase = FAT12_WRITER_DONE;
}

disk_err_t fat12_close_write(fat12_writer_t *writer) {
  if (writer == NULL || writer->fat == NULL) return DISK_ERR_INVALID;
  fat12_t *fat = writer->fat;
  if (writer->phase == FAT12_WRITER_DONE) return DISK_OK;
  if (writer->error != DISK_OK) return writer->error;

  for (;;) {
    disk_err_t err;
    switch (writer->phase) {
      case FAT12_WRITER_DATA:
        err = fat12_writer_flush_cluster(writer);
        if (err != DISK_OK) return err;
        writer->phase = FAT12_WRITER_FLUSH_NEW;
        break;

      case FAT12_WRITER_FLUSH_NEW:
        err = cache_flush(fat->cache);
        if (err != DISK_OK) return err;
        writer->phase = FAT12_WRITER_STAGE_DIRENT;
        break;

      case FAT12_WRITER_STAGE_DIRENT: {
        fat12_dirent_t published = writer->dirent;
        published.start_cluster = writer->first_cluster;
        published.size = writer->bytes_written;
        err = fat12_write_root_entry(fat, writer->dirent_index, &published);
        if (err == DISK_OK && writer->consumed_end &&
            writer->dirent_index + 1u < FAT12_ROOT_ENTRIES) {
          fat12_dirent_t end;
          memset(&end, 0, sizeof(end));
          err = fat12_write_root_entry(fat, writer->dirent_index + 1u, &end);
        }
        if (err != DISK_OK) return err;
        writer->phase = FAT12_WRITER_FLUSH_DIRENT;
        break;
      }

      case FAT12_WRITER_FLUSH_DIRENT:
        err = cache_flush(fat->cache);
        if (err != DISK_OK) return err;
        writer->phase = FAT12_WRITER_PREPARE_RECLAIM;
        break;

      case FAT12_WRITER_PREPARE_RECLAIM:
        if (writer->dirent.start_cluster < 2) {
          writer->phase = FAT12_WRITER_DONE;
          fat12_transaction_end(fat);
          return DISK_OK;
        }
        err = fat12_stage_free_chain(fat, writer->dirent.start_cluster);
        if (err != DISK_OK) return err;
        writer->phase = FAT12_WRITER_FLUSH_RECLAIM;
        break;

      case FAT12_WRITER_FLUSH_RECLAIM:
        err = cache_flush(fat->cache);
        if (err != DISK_OK) return err;
        writer->phase = FAT12_WRITER_DONE;
        fat12_transaction_end(fat);
        return DISK_OK;

      case FAT12_WRITER_DONE:
        return DISK_OK;
    }
  }
}

static disk_err_t fat12_lookup_file(fat12_t *fat, const char *filename,
                                    fat12_lookup_t *hit) {
  fat12_name_t name;
  disk_err_t err = fat12_name_parse(filename, &name);
  if (err != DISK_OK) return err;
  err = fat12_root_lookup(fat, &name, hit);
  if (err != DISK_OK) return err;
  if (!hit->found) return DISK_ERR_NOT_FOUND;
  if ((hit->entry.attr & FAT12_ATTR_DIRECTORY) != 0) return DISK_ERR_IS_DIR;
  if ((hit->entry.attr & FAT12_ATTR_READ_ONLY) != 0) return DISK_ERR_READ_ONLY;
  return DISK_OK;
}

disk_err_t fat12_delete(fat12_t *fat, const char *filename) {
  if (fat == NULL) return DISK_ERR_INVALID;
  disk_err_t err = fat12_mutable(fat);
  if (err != DISK_OK) return err;
  fat12_lookup_t hit;
  err = fat12_lookup_file(fat, filename, &hit);
  if (err != DISK_OK) return err;
  err = fat12_require_exclusive_chains(fat);
  if (err != DISK_OK) return err;
  err = fat12_validate_entry_chain(fat, &hit.entry);
  if (err != DISK_OK) return err;

  fat->transaction = true;
  uint16_t start = hit.entry.start_cluster;
  hit.entry.name[0] = (char)FAT12_DIRENT_FREE;
  err = fat12_write_root_entry(fat, hit.index, &hit.entry);
  if (err == DISK_OK) err = cache_flush(fat->cache);
  if (err == DISK_OK && start >= 2) {
    err = fat12_stage_free_chain(fat, start);
    if (err == DISK_OK) err = cache_flush(fat->cache);
  }
  fat12_transaction_end(fat);
  return err;
}

disk_err_t fat12_rename(fat12_t *fat, const char *from, const char *to) {
  if (fat == NULL) return DISK_ERR_INVALID;
  disk_err_t err = fat12_mutable(fat);
  if (err != DISK_OK) return err;
  fat12_name_t target;
  err = fat12_name_parse(to, &target);
  if (err != DISK_OK) return err;
  fat12_lookup_t existing;
  err = fat12_root_lookup(fat, &target, &existing);
  if (err != DISK_OK) return err;
  if (existing.found) return DISK_ERR_EXISTS;
  fat12_lookup_t hit;
  err = fat12_lookup_file(fat, from, &hit);
  if (err != DISK_OK) return err;

  fat->transaction = true;
  memcpy(hit.entry.name, target.name, sizeof(hit.entry.name));
  memcpy(hit.entry.ext, target.ext, sizeof(hit.entry.ext));
  err = fat12_write_root_entry(fat, hit.index, &hit.entry);
  if (err == DISK_OK) err = cache_flush(fat->cache);
  fat12_transaction_end(fat);
  return err;
}
