#include "fat12.h"
#include <string.h>
#include <stdlib.h>

static bool fat12_read_sector_batched(fat12_t *fat, uint16_t lba, sector_t *sector);

static void fat12_compute_layout(fat12_t *fat) {
  fat->fat_start_sector = fat->bpb.reserved_sectors;
  fat->root_dir_start_sector = fat->fat_start_sector +
                                (fat->bpb.num_fats * fat->bpb.sectors_per_fat);
  fat->root_dir_sectors = (fat->bpb.root_entries * FAT12_DIR_ENTRY_SIZE +
                           SECTOR_SIZE - 1) / SECTOR_SIZE;
  fat->data_start_sector = fat->root_dir_start_sector + fat->root_dir_sectors;
  fat->total_clusters = (fat->bpb.total_sectors - fat->data_start_sector) /
                         fat->bpb.sectors_per_cluster;
}

static bool fat12_layout_valid(fat12_t *fat) {
  uint32_t fat_end = (uint32_t)fat->fat_start_sector +
                     (uint32_t)fat->bpb.num_fats * fat->bpb.sectors_per_fat;
  uint32_t root_end = (uint32_t)fat->root_dir_start_sector + fat->root_dir_sectors;
  uint32_t per_cyl = (uint32_t)fat->bpb.num_heads * fat->bpb.sectors_per_track;

  if (fat->root_dir_start_sector != fat_end) return false;
  if (fat->data_start_sector != root_end) return false;
  if (root_end >= fat->bpb.total_sectors) return false;
  if (per_cyl == 0) return false;
  if (fat->bpb.total_sectors % per_cyl != 0) return false;
  if (fat->bpb.total_sectors > FLOPPY_TRACKS * fat->bpb.num_heads * fat->bpb.sectors_per_track) return false;
  return fat->total_clusters > 0;
}

static bool fat12_read_sector(fat12_t *fat, uint16_t lba, sector_t *sector) {
  fat12_lba_to_chs(&fat->bpb, lba, &sector->track, &sector->side, &sector->sector_n);
  sector->valid = false;
  return fat->io.read(fat->io.ctx, sector);
}

fat12_err_t fat12_init(fat12_t *fat, fat12_io_t io) {
  memset(fat, 0, sizeof(*fat));
  fat->io = io;

  if (io.read == NULL) {
    return FAT12_ERR_INVALID;
  }

  sector_t boot = { .track = 0, .side = 0, .sector_n = 1, .valid = false };
  if (!fat->io.read(fat->io.ctx, &boot)) {
    return FAT12_ERR_READ;
  }

  uint8_t *b = boot.data;
  if (b[FAT12_BOOT_SIG_OFFSET] != 0x55 || b[FAT12_BOOT_SIG_OFFSET + 1] != 0xAA) {
    return FAT12_ERR_INVALID;
  }

  memcpy(&fat->bpb, &b[FAT12_BPB_OFFSET], sizeof(fat->bpb));

  if (fat->bpb.bytes_per_sector != SECTOR_SIZE ||
      fat->bpb.sectors_per_cluster == 0 ||
      fat->bpb.sectors_per_cluster > FAT12_MAX_CLUSTER_SECTORS ||
      fat->bpb.num_fats == 0 ||
      fat->bpb.sectors_per_fat == 0 ||
      fat->bpb.root_entries == 0 ||
      fat->bpb.total_sectors == 0 ||
      fat->bpb.sectors_per_track == 0 ||
      fat->bpb.num_heads == 0) {
    return FAT12_ERR_INVALID;
  }

  fat12_compute_layout(fat);
  if (!fat12_layout_valid(fat)) {
    return FAT12_ERR_INVALID;
  }

  if (fat->bpb.num_fats >= 2) {
    for (uint16_t s = 0; s < fat->bpb.sectors_per_fat; s++) {
      sector_t fat1, fat2;
      if (fat12_read_sector(fat, fat->fat_start_sector + s, &fat1) &&
          fat12_read_sector(fat, fat->fat_start_sector + fat->bpb.sectors_per_fat + s, &fat2) &&
          memcmp(fat1.data, fat2.data, SECTOR_SIZE) != 0) {
        fat->fat_mismatch = true;
        break;
      }
    }
  }

  return FAT12_OK;
}

static fat12_err_t fat12_resolve_entry(uint16_t cluster, uint16_t total_clusters,
                                        uint16_t fat_start, uint16_t sectors_per_fat,
                                        bool (*read_fn)(void *, uint16_t, sector_t *),
                                        void *read_ctx, uint16_t *next) {
  uint32_t max_cluster = total_clusters + 2;
  if (cluster >= max_cluster && cluster < 0xFF0) {
    *next = 0;
    return FAT12_ERR_INVALID;
  }

  uint32_t fat_offset = cluster + (cluster / 2);
  uint32_t fat_size_bytes = (uint32_t)sectors_per_fat * SECTOR_SIZE;
  if (fat_offset + 1 >= fat_size_bytes) {
    *next = 0;
    return FAT12_ERR_INVALID;
  }

  uint16_t fat_sector = fat_start + (fat_offset / SECTOR_SIZE);
  uint16_t entry_offset = fat_offset % SECTOR_SIZE;

  sector_t sector;
  if (!read_fn(read_ctx, fat_sector, &sector)) {
    return FAT12_ERR_READ;
  }

  uint16_t value;
  if (entry_offset == SECTOR_SIZE - 1) {
    value = sector.data[entry_offset];
    sector_t sector2;
    if (!read_fn(read_ctx, fat_sector + 1, &sector2)) {
      return FAT12_ERR_READ;
    }
    value |= sector2.data[0] << 8;
  } else {
    value = sector.data[entry_offset] | (sector.data[entry_offset + 1] << 8);
  }

  *next = (cluster & 1) ? (value >> 4) : (value & 0x0FFF);
  return FAT12_OK;
}

static bool fat12_read_sector_direct_fn(void *ctx, uint16_t lba, sector_t *out) {
  return fat12_read_sector((fat12_t *)ctx, lba, out);
}

static bool fat12_read_sector_batched_fn(void *ctx, uint16_t lba, sector_t *out) {
  return fat12_read_sector_batched((fat12_t *)ctx, lba, out);
}

fat12_err_t fat12_get_entry(fat12_t *fat, uint16_t cluster, uint16_t *next) {
  return fat12_resolve_entry(cluster, fat->total_clusters,
                              fat->fat_start_sector, fat->bpb.sectors_per_fat,
                              fat12_read_sector_direct_fn, fat, next);
}

bool fat12_is_eof(uint16_t cluster) {
  return cluster >= 0xFF8;
}

static bool fat12_is_free(uint16_t cluster) {
  return cluster == 0;
}

static bool fat12_is_bad(uint16_t cluster) {
  return cluster == 0xFF7;
}

static uint16_t fat12_cluster_to_lba(fat12_t *fat, uint16_t cluster) {
  return fat->data_start_sector + (cluster - 2) * fat->bpb.sectors_per_cluster;
}

fat12_err_t fat12_read_root_entry(fat12_t *fat, uint16_t index, fat12_dirent_t *entry) {
  if (index >= fat->bpb.root_entries) {
    return FAT12_ERR_EOF;
  }

  uint16_t sector_lba = fat->root_dir_start_sector +
                        (index * FAT12_DIR_ENTRY_SIZE) / SECTOR_SIZE;
  uint16_t offset = (index * FAT12_DIR_ENTRY_SIZE) % SECTOR_SIZE;

  sector_t sector;
  if (!fat12_read_sector(fat, sector_lba, &sector)) {
    return FAT12_ERR_READ;
  }

  memcpy(entry, &sector.data[offset], sizeof(*entry));
  return FAT12_OK;
}

bool fat12_entry_valid(fat12_dirent_t *entry) {
  uint8_t first = (uint8_t)entry->name[0];
  if (first == FAT12_DIRENT_END || first == FAT12_DIRENT_FREE) return false;
  if (entry->attr == FAT12_ATTR_LFN) return false;
  return true;
}

bool fat12_entry_is_end(fat12_dirent_t *entry) {
  return (uint8_t)entry->name[0] == FAT12_DIRENT_END;
}

static char fat12_toupper(char c) {
  return (c >= 'a' && c <= 'z') ? (c - 32) : c;
}

static bool fat12_name_char_valid(char c) {
  if (c >= 'A' && c <= 'Z') return true;
  if (c >= 'a' && c <= 'z') return true;
  if (c >= '0' && c <= '9') return true;
  switch (c) {
    case '$': case '%': case '\'': case '-': case '_': case '@': case '~':
    case '`': case '!': case '(': case ')': case '{': case '}': case '^':
    case '#': case '&':
      return true;
    default:
      return false;
  }
}

static bool fat12_format_name(const char *input, char *name8, char *ext3) {
  memset(name8, ' ', 8);
  memset(ext3, ' ', 3);

  if (!input || *input == '\0' || *input == '.') return false;

  int i = 0;
  while (*input && *input != '.' && i < 8) {
    if (!fat12_name_char_valid(*input)) return false;
    name8[i++] = fat12_toupper(*input);
    input++;
  }

  if (i == 0) return false;
  if (*input && *input != '.') return false;

  if (*input == '.') input++;

  i = 0;
  while (*input && i < 3) {
    if (*input == '.' || !fat12_name_char_valid(*input)) return false;
    ext3[i++] = fat12_toupper(*input);
    input++;
  }

  if (*input) return false;
  return true;
}

fat12_err_t fat12_find(fat12_t *fat, const char *filename, fat12_dirent_t *entry) {
  char name8[8], ext3[3];
  if (!fat12_format_name(filename, name8, ext3)) return FAT12_ERR_INVALID;

  for (uint16_t i = 0; i < fat->bpb.root_entries; i++) {
    fat12_err_t err = fat12_read_root_entry(fat, i, entry);
    if (err != FAT12_OK) return err;

    if (fat12_entry_is_end(entry)) {
      return FAT12_ERR_NOT_FOUND;
    }

    if (!fat12_entry_valid(entry)) continue;

    if (memcmp(entry->name, name8, 8) == 0 &&
        memcmp(entry->ext, ext3, 3) == 0) {
      return FAT12_OK;
    }
  }

  return FAT12_ERR_NOT_FOUND;
}

fat12_err_t fat12_read_cluster(fat12_t *fat, uint16_t cluster, uint8_t *buf) {
  if (cluster < 2 || fat12_is_eof(cluster) || fat12_is_bad(cluster)) {
    return FAT12_ERR_INVALID;
  }

  if (cluster >= fat->total_clusters + 2) {
    return FAT12_ERR_INVALID;
  }

  uint16_t lba = fat12_cluster_to_lba(fat, cluster);
  sector_t sector;

  for (uint8_t i = 0; i < fat->bpb.sectors_per_cluster; i++) {
    if (!fat12_read_sector(fat, lba + i, &sector)) {
      return FAT12_ERR_READ;
    }
    memcpy(buf + i * SECTOR_SIZE, sector.data, SECTOR_SIZE);
  }

  return FAT12_OK;
}

fat12_err_t fat12_open(fat12_t *fat, fat12_dirent_t *entry, fat12_file_t *file) {
  if (entry->attr & FAT12_ATTR_DIRECTORY) {
    return FAT12_ERR_INVALID;
  }

  file->fat = fat;
  file->start_cluster = entry->start_cluster;
  file->current_cluster = entry->start_cluster;
  file->buffer_cluster = 0;
  file->file_size = entry->size;
  file->bytes_read = 0;
  file->buffer_valid = false;

  return FAT12_OK;
}

fat12_err_t fat12_seek(fat12_file_t *file, uint32_t offset) {
  fat12_t *fat = file->fat;
  uint16_t cluster_size = fat->bpb.sectors_per_cluster * SECTOR_SIZE;

  if (offset > file->file_size) offset = file->file_size;

  uint16_t cluster = file->start_cluster;
  uint32_t pos = 0;
  uint16_t limit = fat->total_clusters;

  while (pos + cluster_size <= offset &&
         cluster >= 2 && !fat12_is_eof(cluster) && !fat12_is_bad(cluster) &&
         limit-- > 0) {
    uint16_t next;
    fat12_err_t err = fat12_get_entry(fat, cluster, &next);
    if (err != FAT12_OK) return err;
    cluster = next;
    pos += cluster_size;
  }

  file->current_cluster = cluster;
  file->bytes_read = offset;
  file->buffer_valid = false;
  return FAT12_OK;
}

static fat12_err_t fat12_read_current_cluster(fat12_file_t *file) {
  if (!file->buffer_valid || file->buffer_cluster != file->current_cluster) {
    fat12_err_t err = fat12_read_cluster(file->fat, file->current_cluster, file->cluster_buf);
    if (err != FAT12_OK) return err;
    file->buffer_cluster = file->current_cluster;
    file->buffer_valid = true;
  }
  return FAT12_OK;
}

int fat12_read(fat12_file_t *file, uint8_t *buf, size_t len) {
  fat12_t *fat = file->fat;
  size_t cluster_size = fat->bpb.sectors_per_cluster * SECTOR_SIZE;
  size_t total_read = 0;
  uint16_t clusters_walked = 0;

  if (cluster_size > FAT12_MAX_CLUSTER_SECTORS * SECTOR_SIZE) {
    return -FAT12_ERR_INVALID;
  }

  while (len > 0 && file->bytes_read < file->file_size) {
    if (file->current_cluster < 2 || fat12_is_eof(file->current_cluster)) {
      break;
    }
    if (clusters_walked++ >= fat->total_clusters) {
      break;
    }

    fat12_err_t err = fat12_read_current_cluster(file);
    if (err != FAT12_OK) return -err;

    size_t offset_in_cluster = file->bytes_read % cluster_size;
    size_t remaining_in_cluster = cluster_size - offset_in_cluster;
    size_t remaining_in_file = (size_t)file->file_size - file->bytes_read;

    size_t to_copy = len;
    if (to_copy > remaining_in_cluster) to_copy = remaining_in_cluster;
    if (to_copy > remaining_in_file) to_copy = remaining_in_file;

    memcpy(buf, file->cluster_buf + offset_in_cluster, to_copy);
    buf += to_copy;
    len -= to_copy;
    file->bytes_read += (uint32_t)to_copy;
    total_read += to_copy;

    if ((file->bytes_read % cluster_size) == 0) {
      uint16_t next = 0;
      err = fat12_get_entry(fat, file->current_cluster, &next);
      if (err != FAT12_OK) return -err;
      file->current_cluster = next;
      file->buffer_valid = false;
    }
  }

  return (int)total_read;
}

static bool fat12_batch_begin(fat12_t *fat) {
  if (fat->batch.active) return false;
  fat->batch.count = 0;
  fat->batch.active = true;
  return true;
}

void fat12_abort_write(fat12_t *fat) {
  if (!fat) return;
  fat->batch.count = 0;
  fat->batch.active = false;
}

static fat12_err_t fat12_write_batch_add(fat12_write_batch_t *batch, uint16_t lba, const uint8_t *data) {
  for (uint8_t i = 0; i < batch->count; i++) {
    if (batch->lbas[i] == lba) {
      memcpy(batch->data[i], data, SECTOR_SIZE);
      return FAT12_OK;
    }
  }
  if (batch->count >= FAT12_WRITE_BATCH_MAX) {
    return FAT12_ERR_FULL;
  }
  batch->lbas[batch->count] = lba;
  memcpy(batch->data[batch->count], data, SECTOR_SIZE);
  batch->count++;
  return FAT12_OK;
}

static fat12_err_t fat12_write_batch_flush(fat12_t *fat) {
  fat12_write_batch_t *batch = &fat->batch;
  if (batch->count == 0) return FAT12_OK;

  for (uint8_t i = 1; i < batch->count; i++) {
    uint16_t key = batch->lbas[i];
    uint8_t tmp[SECTOR_SIZE];
    memcpy(tmp, batch->data[i], SECTOR_SIZE);
    int j = i - 1;
    while (j >= 0 && batch->lbas[j] > key) {
      batch->lbas[j + 1] = batch->lbas[j];
      memcpy(batch->data[j + 1], batch->data[j], SECTOR_SIZE);
      j--;
    }
    batch->lbas[j + 1] = key;
    memcpy(batch->data[j + 1], tmp, SECTOR_SIZE);
  }

  while (batch->count > 0) {
    uint8_t c, h, s;
    fat12_lba_to_chs(&fat->bpb, batch->lbas[0], &c, &h, &s);

    track_t *track = &fat->write_track;
    memset(track, 0, sizeof(*track));
    track->track = c;
    track->side = h;

    for (int i = 0; i < SECTORS_PER_TRACK; i++) {
      track->sectors[i].track = c;
      track->sectors[i].side = h;
      track->sectors[i].sector_n = i + 1;
      track->sectors[i].valid = false;
    }

    uint8_t new_count = 0;
    for (uint8_t i = 0; i < batch->count; i++) {
      uint8_t bc, bh, bs;
      fat12_lba_to_chs(&fat->bpb, batch->lbas[i], &bc, &bh, &bs);

      if (bc == c && bh == h && bs >= 1 && bs <= SECTORS_PER_TRACK) {
        int idx = bs - 1;
        memcpy(track->sectors[idx].data, batch->data[i], SECTOR_SIZE);
        track->sectors[idx].valid = true;
        track->sectors[idx].size_code = 2;
      } else if (bc != c || bh != h) {
        if (new_count != i) {
          batch->lbas[new_count] = batch->lbas[i];
          memcpy(batch->data[new_count], batch->data[i], SECTOR_SIZE);
        }
        new_count++;
      }
    }
    batch->count = new_count;

    if (!fat->io.write(fat->io.ctx, track)) {
      return FAT12_ERR_WRITE;
    }
  }

  return FAT12_OK;
}

static fat12_err_t fat12_write_sector_batched(fat12_t *fat,
                                              uint16_t lba, const uint8_t *data) {
  fat12_err_t err = fat12_write_batch_add(&fat->batch, lba, data);
  if (err == FAT12_ERR_FULL) {
    err = fat12_write_batch_flush(fat);
    if (err != FAT12_OK) return err;
    return fat12_write_batch_add(&fat->batch, lba, data);
  }
  return err;
}

static bool fat12_read_sector_batched(fat12_t *fat, uint16_t lba, sector_t *sector) {
  fat12_write_batch_t *batch = &fat->batch;
  for (int i = batch->count - 1; i >= 0; i--) {
    if (batch->lbas[i] == lba) {
      memcpy(sector->data, batch->data[i], SECTOR_SIZE);
      sector->valid = true;
      return true;
    }
  }
  return fat12_read_sector(fat, lba, sector);
}

static fat12_err_t fat12_set_entry(fat12_t *fat, uint16_t cluster, uint16_t value) {
  uint32_t fat_offset = cluster + (cluster / 2);
  uint16_t fat_sector_lba = fat->fat_start_sector + (fat_offset / SECTOR_SIZE);
  uint16_t entry_offset = fat_offset % SECTOR_SIZE;

  sector_t sector;
  if (!fat12_read_sector_batched(fat, fat_sector_lba, &sector)) {
    return FAT12_ERR_READ;
  }

  if (entry_offset == SECTOR_SIZE - 1) {
    sector_t sector2;
    if (!fat12_read_sector_batched(fat, fat_sector_lba + 1, &sector2)) {
      return FAT12_ERR_READ;
    }

    if (cluster & 1) {
      sector.data[entry_offset] = (sector.data[entry_offset] & 0x0F) | ((value & 0x0F) << 4);
      sector2.data[0] = value >> 4;
    } else {
      sector.data[entry_offset] = value & 0xFF;
      sector2.data[0] = (sector2.data[0] & 0xF0) | ((value >> 8) & 0x0F);
    }

    for (uint8_t f = 0; f < fat->bpb.num_fats; f++) {
      uint16_t fat_base = fat->fat_start_sector + f * fat->bpb.sectors_per_fat;
      uint16_t lba1 = fat_base + (fat_offset / SECTOR_SIZE);

      fat12_err_t err = fat12_write_sector_batched(fat, lba1, sector.data);
      if (err != FAT12_OK) return err;

      err = fat12_write_sector_batched(fat, lba1 + 1, sector2.data);
      if (err != FAT12_OK) return err;
    }
  } else {
    uint16_t existing = sector.data[entry_offset] | (sector.data[entry_offset + 1] << 8);

    if (cluster & 1) {
      existing = (existing & 0x000F) | (value << 4);
    } else {
      existing = (existing & 0xF000) | (value & 0x0FFF);
    }

    sector.data[entry_offset] = existing & 0xFF;
    sector.data[entry_offset + 1] = existing >> 8;

    for (uint8_t f = 0; f < fat->bpb.num_fats; f++) {
      uint16_t fat_base = fat->fat_start_sector + f * fat->bpb.sectors_per_fat;
      uint16_t lba = fat_base + (fat_offset / SECTOR_SIZE);

      fat12_err_t err = fat12_write_sector_batched(fat, lba, sector.data);
      if (err != FAT12_OK) return err;
    }
  }

  return FAT12_OK;
}

static fat12_err_t fat12_get_entry_batched(fat12_t *fat, uint16_t cluster, uint16_t *next) {
  return fat12_resolve_entry(cluster, fat->total_clusters,
                              fat->fat_start_sector, fat->bpb.sectors_per_fat,
                              fat12_read_sector_batched_fn, fat, next);
}

static fat12_err_t fat12_free_chain(fat12_t *fat, uint16_t start) {
  uint16_t cluster = start;
  uint16_t limit = fat->total_clusters;
  while (cluster >= 2 && !fat12_is_eof(cluster) && !fat12_is_bad(cluster) && limit-- > 0) {
    uint16_t next;
    fat12_err_t err = fat12_get_entry_batched(fat, cluster, &next);
    if (err != FAT12_OK) break;

    err = fat12_set_entry(fat, cluster, 0);
    if (err != FAT12_OK) return err;

    cluster = next;
  }
  return FAT12_OK;
}

static fat12_err_t fat12_find_free_cluster_from(fat12_t *fat,
                                                 uint16_t start, uint16_t *out) {
  if (start < 2) start = 2;

  sector_t sec, sec2;
  uint16_t cached_lba = 0xFFFF;
  uint16_t cached_lba2 = 0xFFFF;

  for (uint16_t cluster = start; cluster < fat->total_clusters + 2; cluster++) {
    uint32_t fat_offset = cluster + (cluster / 2);
    uint16_t lba = fat->fat_start_sector + (fat_offset / SECTOR_SIZE);
    uint16_t offset = fat_offset % SECTOR_SIZE;

    if (lba != cached_lba) {
      if (!fat12_read_sector_batched(fat, lba, &sec))
        return FAT12_ERR_READ;
      cached_lba = lba;
      cached_lba2 = 0xFFFF;
    }

    uint16_t value;
    if (offset == SECTOR_SIZE - 1) {
      if (lba + 1 != cached_lba2) {
        if (!fat12_read_sector_batched(fat, lba + 1, &sec2))
          return FAT12_ERR_READ;
        cached_lba2 = lba + 1;
      }
      value = sec.data[offset] | (sec2.data[0] << 8);
    } else {
      value = sec.data[offset] | (sec.data[offset + 1] << 8);
    }

    uint16_t entry = (cluster & 1) ? (value >> 4) : (value & 0x0FFF);
    if (fat12_is_free(entry)) {
      *out = cluster;
      return FAT12_OK;
    }
  }
  return FAT12_ERR_FULL;
}

static fat12_err_t fat12_count_chain_clusters(fat12_t *fat,
                                              uint16_t start, uint16_t *count) {
  uint16_t total = 0;
  uint16_t cluster = start;
  uint16_t limit = fat->total_clusters;

  while (cluster >= 2 && !fat12_is_eof(cluster) && !fat12_is_bad(cluster) && limit-- > 0) {
    uint16_t next;
    fat12_err_t err = fat12_get_entry_batched(fat, cluster, &next);
    if (err != FAT12_OK) return err;
    total++;
    cluster = next;
  }

  *count = total;
  return FAT12_OK;
}

static fat12_err_t fat12_has_free_clusters(fat12_t *fat, uint16_t needed, bool *out) {
  uint16_t found = 0;

  if (needed == 0) {
    *out = true;
    return FAT12_OK;
  }

  for (uint16_t cluster = 2; cluster < fat->total_clusters + 2; cluster++) {
    uint16_t entry;
    fat12_err_t err = fat12_get_entry_batched(fat, cluster, &entry);
    if (err != FAT12_OK) return err;

    if (fat12_is_free(entry) && ++found >= needed) {
      *out = true;
      return FAT12_OK;
    }
  }

  *out = false;
  return FAT12_OK;
}

static fat12_err_t fat12_write_cluster(fat12_t *fat,
                                       uint16_t cluster, const uint8_t *buf) {
  if (cluster < 2 || fat12_is_eof(cluster) || fat12_is_bad(cluster)) {
    return FAT12_ERR_INVALID;
  }

  if (cluster >= fat->total_clusters + 2) {
    return FAT12_ERR_INVALID;
  }

  uint16_t lba = fat12_cluster_to_lba(fat, cluster);

  for (uint8_t i = 0; i < fat->bpb.sectors_per_cluster; i++) {
    fat12_err_t err = fat12_write_sector_batched(fat, lba + i, buf + i * SECTOR_SIZE);
    if (err != FAT12_OK) return err;
  }

  return FAT12_OK;
}

static fat12_err_t fat12_write_root_entry(fat12_t *fat,
                                           uint16_t index,
                                           fat12_dirent_t *entry) {
  if (index >= fat->bpb.root_entries) {
    return FAT12_ERR_EOF;
  }

  uint16_t sector_lba = fat->root_dir_start_sector +
                        (index * FAT12_DIR_ENTRY_SIZE) / SECTOR_SIZE;
  uint16_t offset = (index * FAT12_DIR_ENTRY_SIZE) % SECTOR_SIZE;

  sector_t sector;
  if (!fat12_read_sector_batched(fat, sector_lba, &sector)) {
    return FAT12_ERR_READ;
  }

  memcpy(&sector.data[offset], entry, sizeof(*entry));

  return fat12_write_sector_batched(fat, sector_lba, sector.data);
}

static fat12_err_t fat12_find_free_dirent(fat12_t *fat, uint16_t *index) {
  fat12_dirent_t entry;

  for (uint16_t i = 0; i < fat->bpb.root_entries; i++) {
    fat12_err_t err = fat12_read_root_entry(fat, i, &entry);
    if (err != FAT12_OK) return err;

    uint8_t first = (uint8_t)entry.name[0];
    if (first == FAT12_DIRENT_END || first == FAT12_DIRENT_FREE) {
      *index = i;
      return FAT12_OK;
    }
  }
  return FAT12_ERR_FULL;
}

fat12_err_t fat12_create(fat12_t *fat, const char *filename, fat12_dirent_t *entry) {
  fat12_dirent_t existing;
  fat12_err_t err = fat12_find(fat, filename, &existing);
  if (err == FAT12_OK) {
    return FAT12_ERR_INVALID;
  }
  if (err != FAT12_ERR_NOT_FOUND) {
    return err;
  }

  uint16_t dirent_idx;
  err = fat12_find_free_dirent(fat, &dirent_idx);
  if (err != FAT12_OK) return err;

  memset(entry, 0, sizeof(*entry));
  fat12_format_name(filename, entry->name, entry->ext);
  entry->attr = FAT12_ATTR_ARCHIVE;
  entry->start_cluster = 0;
  entry->size = 0;

  if (fat->batch.active) return FAT12_ERR_INVALID;
  if (!fat12_batch_begin(fat)) return FAT12_ERR_READ;

  err = fat12_write_root_entry(fat, dirent_idx, entry);
  if (err == FAT12_OK) {
    err = fat12_write_batch_flush(fat);
  }
  fat12_abort_write(fat);
  return err;
}

static void fat12_init_dirent(fat12_dirent_t *d, const char *name8, const char *ext3) {
  memset(d, 0, sizeof(*d));
  memcpy(d->name, name8, 8);
  memcpy(d->ext, ext3, 3);
  d->attr = FAT12_ATTR_ARCHIVE;
}

fat12_err_t fat12_open_write(fat12_t *fat, const char *filename, fat12_writer_t *writer) {
  if (fat->batch.active) return FAT12_ERR_INVALID;

  fat12_err_t result = FAT12_ERR_FULL;

  memset(writer, 0, sizeof(*writer));
  writer->fat = fat;
  if (!fat12_batch_begin(fat)) {
    result = FAT12_ERR_READ;
    goto fail;
  }

  char name8[8], ext3[3];
  if (!fat12_format_name(filename, name8, ext3)) {
    result = FAT12_ERR_INVALID;
    goto fail;
  }

  for (uint16_t i = 0; i < fat->bpb.root_entries; i++) {
    fat12_err_t err = fat12_read_root_entry(fat, i, &writer->dirent);
    if (err != FAT12_OK) {
      result = err;
      goto fail;
    }

    if (fat12_entry_is_end(&writer->dirent)) {
      writer->dirent_index = i;
      fat12_init_dirent(&writer->dirent, name8, ext3);
      return FAT12_OK;
    }

    uint8_t first = (uint8_t)writer->dirent.name[0];
    if (first == FAT12_DIRENT_FREE) {
      writer->dirent_index = i;
      fat12_init_dirent(&writer->dirent, name8, ext3);
      return FAT12_OK;
    }

    if (memcmp(writer->dirent.name, name8, 8) == 0 &&
        memcmp(writer->dirent.ext, ext3, 3) == 0) {
      writer->dirent_index = i;
      writer->old_start_cluster = writer->dirent.start_cluster;
      if (writer->old_start_cluster >= 2) {
        uint16_t old_clusters = 0;
        bool can_preserve_old = false;

        err = fat12_count_chain_clusters(fat, writer->old_start_cluster, &old_clusters);
        if (err != FAT12_OK) {
          result = err;
          goto fail;
        }

        err = fat12_has_free_clusters(fat, old_clusters, &can_preserve_old);
        if (err != FAT12_OK) {
          result = err;
          goto fail;
        }

        if (can_preserve_old) {
          writer->replacing_existing = true;
        } else {
          writer->overwrite_in_place = true;
          writer->first_cluster = writer->old_start_cluster;
          writer->current_cluster = writer->old_start_cluster;
          writer->cluster_loaded = true;
          memset(writer->cluster_buf, 0, sizeof(writer->cluster_buf));
        }
      }
      return FAT12_OK;
    }
  }

fail:
  fat12_abort_write(fat);
  memset(writer, 0, sizeof(*writer));
  return result;
}

static fat12_err_t fat12_writer_alloc_cluster(fat12_writer_t *writer) {
  fat12_t *fat = writer->fat;
  uint16_t new_cluster;
  fat12_err_t err = fat12_find_free_cluster_from(fat, fat->next_free_hint, &new_cluster);
  if (err != FAT12_OK) return err;

  err = fat12_set_entry(fat, new_cluster, 0xFFF);
  if (err != FAT12_OK) return err;

  if (writer->prev_cluster != 0) {
    err = fat12_set_entry(fat, writer->prev_cluster, new_cluster);
    if (err != FAT12_OK) return err;
  }

  if (writer->first_cluster == 0) {
    writer->first_cluster = new_cluster;
  }

  writer->current_cluster = new_cluster;
  writer->cluster_offset = 0;
  writer->cluster_loaded = true;
  writer->cluster_dirty = false;
  memset(writer->cluster_buf, 0, sizeof(writer->cluster_buf));
  fat->next_free_hint = new_cluster + 1;

  return FAT12_OK;
}

static int fat12_writer_fail(fat12_writer_t *writer, fat12_err_t err) {
  if (writer->error == FAT12_OK) {
    writer->error = err;
  }
  return -err;
}

static fat12_err_t fat12_writer_flush_cluster(fat12_writer_t *writer) {
  if (!writer->cluster_loaded || !writer->cluster_dirty || writer->current_cluster == 0) {
    return FAT12_OK;
  }

  fat12_err_t err = fat12_write_cluster(writer->fat, writer->current_cluster, writer->cluster_buf);
  if (err != FAT12_OK) return err;

  writer->cluster_dirty = false;
  return FAT12_OK;
}

static fat12_err_t fat12_writer_prepare_next_cluster(fat12_writer_t *writer) {
  fat12_err_t err = fat12_writer_flush_cluster(writer);
  if (err != FAT12_OK) return err;

  if (writer->current_cluster != 0) {
    uint16_t last_cluster = writer->current_cluster;

    if (writer->overwrite_in_place) {
      uint16_t next = 0;
      err = fat12_get_entry_batched(writer->fat, last_cluster, &next);
      if (err != FAT12_OK) return err;

      if (next >= 2 && !fat12_is_eof(next) && !fat12_is_bad(next)) {
        writer->prev_cluster = last_cluster;
        writer->current_cluster = next;
        writer->cluster_offset = 0;
        writer->cluster_loaded = true;
        writer->cluster_dirty = false;
        memset(writer->cluster_buf, 0, sizeof(writer->cluster_buf));
        return FAT12_OK;
      }
    }

    writer->prev_cluster = last_cluster;
    writer->current_cluster = 0;
    writer->cluster_loaded = false;
    writer->cluster_dirty = false;
  }

  return fat12_writer_alloc_cluster(writer);
}

int fat12_write(fat12_writer_t *writer, const uint8_t *buf, size_t len) {
  fat12_t *fat = writer->fat;
  size_t cluster_size = fat->bpb.sectors_per_cluster * SECTOR_SIZE;
  size_t total_written = 0;

  if (writer->error != FAT12_OK) {
    return -writer->error;
  }

  if (cluster_size > FAT12_MAX_CLUSTER_SECTORS * SECTOR_SIZE) {
    return fat12_writer_fail(writer, FAT12_ERR_INVALID);
  }

  while (len > 0) {
    if (writer->current_cluster == 0 || writer->cluster_offset >= cluster_size) {
      fat12_err_t err = fat12_writer_prepare_next_cluster(writer);
      if (err != FAT12_OK) return fat12_writer_fail(writer, err);
    }

    size_t remaining_in_cluster = cluster_size - writer->cluster_offset;
    size_t to_write = len;
    if (to_write > remaining_in_cluster) to_write = remaining_in_cluster;

    memcpy(writer->cluster_buf + writer->cluster_offset, buf, to_write);
    writer->cluster_dirty = true;

    buf += to_write;
    len -= to_write;
    writer->bytes_written += (uint32_t)to_write;
    writer->cluster_offset += (uint16_t)to_write;
    total_written += to_write;
  }

  return (int)total_written;
}

static fat12_err_t fat12_writer_finish_in_place(fat12_writer_t *writer) {
  if (!writer->overwrite_in_place || writer->old_start_cluster < 2) {
    return FAT12_OK;
  }

  if (writer->bytes_written == 0) {
    writer->first_cluster = 0;
    if (writer->old_start_cluster < writer->fat->next_free_hint) {
      writer->fat->next_free_hint = writer->old_start_cluster;
    }
    return fat12_free_chain(writer->fat, writer->old_start_cluster);
  }

  uint16_t last_cluster = writer->current_cluster;
  if (last_cluster < 2) {
    last_cluster = writer->prev_cluster;
  }
  if (last_cluster < 2) {
    return FAT12_ERR_INVALID;
  }

  writer->first_cluster = writer->old_start_cluster;

  uint16_t tail_start = 0;
  fat12_err_t err = fat12_get_entry_batched(writer->fat, last_cluster, &tail_start);
  if (err != FAT12_OK) return err;

  if (tail_start >= 2 && !fat12_is_eof(tail_start) && !fat12_is_bad(tail_start)) {
    err = fat12_set_entry(writer->fat, last_cluster, 0xFFF);
    if (err != FAT12_OK) return err;

    err = fat12_free_chain(writer->fat, tail_start);
    if (err != FAT12_OK) return err;

    if (tail_start < writer->fat->next_free_hint) {
      writer->fat->next_free_hint = tail_start;
    }
  }

  return FAT12_OK;
}

fat12_err_t fat12_close_write(fat12_writer_t *writer) {
  if (writer->error != FAT12_OK) {
    fat12_abort_write(writer->fat);
    return writer->error;
  }

  fat12_err_t err = fat12_writer_flush_cluster(writer);
  if (err != FAT12_OK) {
    fat12_abort_write(writer->fat);
    return err;
  }

  err = fat12_writer_finish_in_place(writer);
  if (err != FAT12_OK) {
    fat12_abort_write(writer->fat);
    return err;
  }

  writer->dirent.start_cluster = writer->first_cluster;
  writer->dirent.size = writer->bytes_written;

  err = fat12_write_batch_flush(writer->fat);
  if (err != FAT12_OK) {
    fat12_abort_write(writer->fat);
    return err;
  }

  err = fat12_write_root_entry(writer->fat, writer->dirent_index, &writer->dirent);
  if (err != FAT12_OK) {
    fat12_abort_write(writer->fat);
    return err;
  }

  err = fat12_write_batch_flush(writer->fat);
  if (err != FAT12_OK) {
    fat12_abort_write(writer->fat);
    return err;
  }

  if (writer->replacing_existing && writer->old_start_cluster >= 2) {
    err = fat12_free_chain(writer->fat, writer->old_start_cluster);
    if (err != FAT12_OK) {
      fat12_abort_write(writer->fat);
      return err;
    }

    if (writer->old_start_cluster < writer->fat->next_free_hint) {
      writer->fat->next_free_hint = writer->old_start_cluster;
    }

    err = fat12_write_batch_flush(writer->fat);
    if (err != FAT12_OK) {
      fat12_abort_write(writer->fat);
      return err;
    }
  }

  fat12_abort_write(writer->fat);
  return FAT12_OK;
}

fat12_err_t fat12_delete(fat12_t *fat, const char *filename) {
  if (fat->batch.active) return FAT12_ERR_INVALID;

  fat12_dirent_t entry;
  char name8[8], ext3[3];
  if (!fat12_format_name(filename, name8, ext3)) return FAT12_ERR_INVALID;

  if (!fat12_batch_begin(fat)) return FAT12_ERR_READ;

  fat12_err_t result = FAT12_ERR_NOT_FOUND;

  for (uint16_t i = 0; i < fat->bpb.root_entries; i++) {
    fat12_err_t err = fat12_read_root_entry(fat, i, &entry);
    if (err != FAT12_OK) { result = err; goto done; }

    if (fat12_entry_is_end(&entry)) goto done;
    if (!fat12_entry_valid(&entry)) continue;

    if (memcmp(entry.name, name8, 8) == 0 &&
        memcmp(entry.ext, ext3, 3) == 0) {
      uint16_t start_cluster = entry.start_cluster;
      if (entry.start_cluster >= 2 && entry.start_cluster < fat->next_free_hint) {
        fat->next_free_hint = entry.start_cluster;
      }

      entry.name[0] = FAT12_DIRENT_FREE;
      err = fat12_write_root_entry(fat, i, &entry);
      if (err != FAT12_OK) { result = err; goto done; }

      err = fat12_write_batch_flush(fat);
      if (err != FAT12_OK) { result = err; goto done; }

      err = fat12_free_chain(fat, start_cluster);
      if (err != FAT12_OK) { result = err; goto done; }

      result = fat12_write_batch_flush(fat);
      goto done;
    }
  }

done:
  fat12_abort_write(fat);
  return result;
}

static void fat12_build_boot_sector(uint8_t *boot, const fat12_bpb_t *bpb,
                                    const char *volume_label) {
  memset(boot, 0, SECTOR_SIZE);

  boot[0] = 0xEB; boot[1] = 0x3C; boot[2] = 0x90;
  memcpy(&boot[3], "MSDOS5.0", 8);

  memcpy(&boot[FAT12_BPB_OFFSET], bpb, sizeof(*bpb));

  boot[36] = 0x00; boot[37] = 0x00; boot[38] = 0x29;
  boot[39] = 0x12; boot[40] = 0x34; boot[41] = 0x56; boot[42] = 0x78;

  if (volume_label) {
    int i;
    for (i = 0; i < 11 && volume_label[i]; i++)
      boot[43 + i] = fat12_toupper(volume_label[i]);
    for (; i < 11; i++)
      boot[43 + i] = ' ';
  } else {
    memcpy(&boot[43], "NO NAME    ", 11);
  }

  memcpy(&boot[54], "FAT12   ", 8);
  boot[FAT12_BOOT_SIG_OFFSET] = 0x55;
  boot[FAT12_BOOT_SIG_OFFSET + 1] = 0xAA;
}

static void fat12_build_volume_label(uint8_t *sector, const char *volume_label) {
  memset(sector, 0, SECTOR_SIZE);
  if (!volume_label) return;

  fat12_dirent_t *entry = (fat12_dirent_t *)sector;
  int i;
  for (i = 0; i < 8 && volume_label[i] && volume_label[i] != '.'; i++)
    entry->name[i] = fat12_toupper(volume_label[i]);
  for (; i < 8; i++)
    entry->name[i] = ' ';

  int label_len = 0;
  while (volume_label[label_len]) label_len++;

  int j = 0;
  if (label_len > 8) {
    for (j = 0; j < 3 && (8 + j) < label_len; j++)
      entry->ext[j] = fat12_toupper(volume_label[8 + j]);
  }
  for (; j < 3; j++)
    entry->ext[j] = ' ';

  entry->attr = FAT12_ATTR_VOLUME_ID;
}

static void fat12_init_hd_bpb(fat12_bpb_t *bpb) {
  memset(bpb, 0, sizeof(*bpb));
  bpb->bytes_per_sector    = SECTOR_SIZE;
  bpb->sectors_per_cluster = 1;
  bpb->reserved_sectors    = 1;
  bpb->num_fats            = 2;
  bpb->root_entries        = 224;
  bpb->total_sectors       = 80 * 2 * 18;
  bpb->media_descriptor    = 0xF0;
  bpb->sectors_per_fat     = 9;
  bpb->sectors_per_track   = 18;
  bpb->num_heads           = 2;
}

static void fat12_fill_format_sector(sector_t *s, uint16_t lba,
                                     const fat12_t *fat,
                                     const uint8_t *boot,
                                     const uint8_t *fat_sector,
                                     const uint8_t *root_first,
                                     const char *volume_label,
                                     bool write_all_tracks) {
  uint16_t fat2_start = fat->fat_start_sector + fat->bpb.sectors_per_fat;

  if (lba == 0) {
    memcpy(s->data, boot, SECTOR_SIZE);
  } else if (lba >= fat->fat_start_sector && lba < fat2_start) {
    if (lba == fat->fat_start_sector)
      memcpy(s->data, fat_sector, SECTOR_SIZE);
    else
      memset(s->data, 0, SECTOR_SIZE);
  } else if (lba >= fat2_start && lba < fat2_start + fat->bpb.sectors_per_fat) {
    if (lba == fat2_start)
      memcpy(s->data, fat_sector, SECTOR_SIZE);
    else
      memset(s->data, 0, SECTOR_SIZE);
  } else if (lba >= fat->root_dir_start_sector &&
             lba < fat->root_dir_start_sector + fat->root_dir_sectors) {
    if (lba == fat->root_dir_start_sector && volume_label)
      memcpy(s->data, root_first, SECTOR_SIZE);
    else
      memset(s->data, 0, SECTOR_SIZE);
  } else {
    memset(s->data, 0, SECTOR_SIZE);
    if (!write_all_tracks && lba >= fat->data_start_sector)
      s->valid = false;
  }
}

fat12_err_t fat12_format(fat12_io_t io, const char *volume_label, bool write_all_tracks) {
  if (io.write == NULL)
    return FAT12_ERR_INVALID;

  fat12_t fat;
  memset(&fat, 0, sizeof(fat));
  fat12_init_hd_bpb(&fat.bpb);
  fat12_compute_layout(&fat);

  uint8_t boot[SECTOR_SIZE];
  fat12_build_boot_sector(boot, &fat.bpb, volume_label);

  uint8_t fat_sector[SECTOR_SIZE];
  memset(fat_sector, 0, sizeof(fat_sector));
  fat_sector[0] = fat.bpb.media_descriptor;
  fat_sector[1] = 0xFF;
  fat_sector[2] = 0xFF;

  uint8_t root_first[SECTOR_SIZE];
  fat12_build_volume_label(root_first, volume_label);

  for (uint8_t cyl = 0; cyl < 80; cyl++) {
    for (uint8_t side = 0; side < fat.bpb.num_heads; side++) {
      track_t *t = &fat.write_track;
      memset(t, 0, sizeof(*t));
      t->track = cyl;
      t->side = side;

      bool has_valid = false;
      for (uint8_t s = 0; s < fat.bpb.sectors_per_track; s++) {
        uint16_t lba = fat12_chs_to_lba(&fat.bpb, cyl, side, s + 1);

        t->sectors[s].track = t->track;
        t->sectors[s].side = side;
        t->sectors[s].sector_n = s + 1;
        t->sectors[s].size_code = 2;
        t->sectors[s].valid = true;

        fat12_fill_format_sector(&t->sectors[s], lba, &fat, boot,
                                 fat_sector, root_first, volume_label,
                                 write_all_tracks);

        if (t->sectors[s].valid) has_valid = true;
      }

      if (has_valid) {
        if (!io.write(io.ctx, t))
          return FAT12_ERR_WRITE;
      }

      if (!write_all_tracks) {
        uint16_t track_end_lba = (cyl * fat.bpb.num_heads + side + 1) *
                                 fat.bpb.sectors_per_track;
        if (track_end_lba > fat.data_start_sector)
          return FAT12_OK;
      }
    }
  }

  return FAT12_OK;
}
