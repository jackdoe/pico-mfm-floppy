#include "fat12.h"
#include <string.h>

static bool fat12_read_sector_batched(fat12_write_batch_t *batch,
                                      uint16_t lba, sector_t *sector);

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

static void fat12_lba_to_chs(fat12_t *fat, uint16_t lba, uint8_t *c, uint8_t *h, uint8_t *s) {
  *c = lba / (fat->bpb.num_heads * fat->bpb.sectors_per_track);
  uint16_t temp = lba % (fat->bpb.num_heads * fat->bpb.sectors_per_track);
  *h = temp / fat->bpb.sectors_per_track;
  *s = (temp % fat->bpb.sectors_per_track) + 1;
}

static bool fat12_read_sector(fat12_t *fat, uint16_t lba, sector_t *sector) {
  uint8_t c, h, s;
  fat12_lba_to_chs(fat, lba, &c, &h, &s);
  sector->track = c;
  sector->side = h;
  sector->sector_n = s;
  sector->valid = false;
  return fat->io.read(fat->io.ctx, sector);
}

fat12_err_t fat12_init(fat12_t *fat, fat12_io_t io) {
  memset(fat, 0, sizeof(*fat));
  fat->io = io;

  if (io.read == NULL) {
    return FAT12_ERR_INVALID;
  }

  fat->sector_buf.track = 0;
  fat->sector_buf.side = 0;
  fat->sector_buf.sector_n = 1;
  fat->sector_buf.valid = false;
  if (!fat->io.read(fat->io.ctx, &fat->sector_buf)) {
    return FAT12_ERR_READ;
  }

  uint8_t *b = fat->sector_buf.data;
  if (b[FAT12_BOOT_SIG_OFFSET] != 0x55 || b[FAT12_BOOT_SIG_OFFSET + 1] != 0xAA) {
    return FAT12_ERR_INVALID;
  }

  fat12_bpb_raw_t raw;
  memcpy(&raw, &b[FAT12_BPB_OFFSET], sizeof(raw));
  fat12_bpb_from_raw(&fat->bpb, &raw);

  if (fat->bpb.bytes_per_sector != SECTOR_SIZE ||
      fat->bpb.sectors_per_cluster == 0 ||
      fat->bpb.sectors_per_cluster > FAT12_MAX_CLUSTER_SECTORS ||
      fat->bpb.num_fats == 0 ||
      fat->bpb.sectors_per_track == 0 ||
      fat->bpb.num_heads == 0) {
    return FAT12_ERR_INVALID;
  }

  fat12_compute_layout(fat);

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

static bool fat12_read_sector_into_buf(void *ctx, uint16_t lba, sector_t *out) {
  fat12_t *fat = (fat12_t *)ctx;
  if (!fat12_read_sector(fat, lba, &fat->sector_buf)) return false;
  *out = fat->sector_buf;
  return true;
}

typedef struct {
  fat12_write_batch_t *batch;
} batched_read_ctx_t;

static bool fat12_read_sector_batched_fn(void *ctx, uint16_t lba, sector_t *out) {
  batched_read_ctx_t *bctx = (batched_read_ctx_t *)ctx;
  return fat12_read_sector_batched(bctx->batch, lba, out);
}

fat12_err_t fat12_get_entry(fat12_t *fat, uint16_t cluster, uint16_t *next) {
  return fat12_resolve_entry(cluster, fat->total_clusters,
                              fat->fat_start_sector, fat->bpb.sectors_per_fat,
                              fat12_read_sector_into_buf, fat, next);
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

  uint16_t sector = fat->root_dir_start_sector +
                    (index * FAT12_DIR_ENTRY_SIZE) / SECTOR_SIZE;
  uint16_t offset = (index * FAT12_DIR_ENTRY_SIZE) % SECTOR_SIZE;

  if (!fat12_read_sector(fat, sector, &fat->sector_buf)) {
    return FAT12_ERR_READ;
  }

  memcpy(entry, &fat->sector_buf.data[offset], sizeof(*entry));
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

static void fat12_format_name(const char *input, char *name8, char *ext3) {
  memset(name8, ' ', 8);
  memset(ext3, ' ', 3);

  int i = 0;
  while (*input && *input != '.' && i < 8) {
    name8[i++] = fat12_toupper(*input);
    input++;
  }

  if (*input == '.') input++;

  i = 0;
  while (*input && i < 3) {
    ext3[i++] = fat12_toupper(*input);
    input++;
  }
}

fat12_err_t fat12_find(fat12_t *fat, const char *filename, fat12_dirent_t *entry) {
  char name8[8], ext3[3];
  fat12_format_name(filename, name8, ext3);

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
  file->current_cluster = entry->start_cluster;
  file->file_size = entry->size;
  file->bytes_read = 0;

  return FAT12_OK;
}

int fat12_read(fat12_file_t *file, uint8_t *buf, uint16_t len) {
  fat12_t *fat = file->fat;
  uint16_t cluster_size = fat->bpb.sectors_per_cluster * SECTOR_SIZE;
  uint16_t total_read = 0;

  if (cluster_size > FAT12_MAX_CLUSTER_SECTORS * SECTOR_SIZE) {
    return -FAT12_ERR_INVALID;
  }

  while (len > 0 && file->bytes_read < file->file_size) {
    if (file->current_cluster < 2 || fat12_is_eof(file->current_cluster)) {
      break;
    }

    uint8_t cluster_buf[FAT12_MAX_CLUSTER_SECTORS * SECTOR_SIZE];
    fat12_err_t err = fat12_read_cluster(fat, file->current_cluster, cluster_buf);
    if (err != FAT12_OK) return -err;

    uint16_t offset_in_cluster = file->bytes_read % cluster_size;
    uint16_t remaining_in_cluster = cluster_size - offset_in_cluster;
    uint32_t remaining_in_file = file->file_size - file->bytes_read;

    uint16_t to_copy = len;
    if (to_copy > remaining_in_cluster) to_copy = remaining_in_cluster;
    if (to_copy > remaining_in_file) to_copy = remaining_in_file;

    memcpy(buf, cluster_buf + offset_in_cluster, to_copy);
    buf += to_copy;
    len -= to_copy;
    file->bytes_read += to_copy;
    total_read += to_copy;

    if ((file->bytes_read % cluster_size) == 0) {
      uint16_t next = 0;
      err = fat12_get_entry(fat, file->current_cluster, &next);
      if (err != FAT12_OK) return -err;
      file->current_cluster = next;
    }
  }

  return total_read;
}

static void fat12_write_batch_init(fat12_write_batch_t *batch, fat12_t *fat) {
  batch->fat = fat;
  batch->count = 0;
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

static fat12_err_t fat12_write_batch_flush(fat12_write_batch_t *batch) {
  if (batch->count == 0) return FAT12_OK;

  fat12_t *fat = batch->fat;

  while (batch->count > 0) {
    uint8_t c, h, s;
    fat12_lba_to_chs(fat, batch->lbas[0], &c, &h, &s);

    track_t track;
    memset(&track, 0, sizeof(track));
    track.track = c;
    track.side = h;

    for (int i = 0; i < SECTORS_PER_TRACK; i++) {
      track.sectors[i].track = c;
      track.sectors[i].side = h;
      track.sectors[i].sector_n = i + 1;
      track.sectors[i].valid = false;
    }

    uint8_t new_count = 0;
    for (uint8_t i = 0; i < batch->count; i++) {
      uint8_t bc, bh, bs;
      fat12_lba_to_chs(fat, batch->lbas[i], &bc, &bh, &bs);

      if (bc == c && bh == h && bs >= 1 && bs <= SECTORS_PER_TRACK) {
        int idx = bs - 1;
        memcpy(track.sectors[idx].data, batch->data[i], SECTOR_SIZE);
        track.sectors[idx].valid = true;
        track.sectors[idx].size_code = 2;
      } else if (bc != c || bh != h) {
        if (new_count != i) {
          batch->lbas[new_count] = batch->lbas[i];
          memcpy(batch->data[new_count], batch->data[i], SECTOR_SIZE);
        }
        new_count++;
      }
    }
    batch->count = new_count;

    if (!fat->io.write(fat->io.ctx, &track)) {
      return FAT12_ERR_WRITE;
    }
  }

  return FAT12_OK;
}

static fat12_err_t fat12_write_sector_batched(fat12_write_batch_t *batch,
                                              uint16_t lba, const uint8_t *data) {
  fat12_err_t err = fat12_write_batch_add(batch, lba, data);
  if (err == FAT12_ERR_FULL) {
    err = fat12_write_batch_flush(batch);
    if (err != FAT12_OK) return err;
    return fat12_write_batch_add(batch, lba, data);
  }
  return err;
}

static bool fat12_read_sector_batched(fat12_write_batch_t *batch,
                                      uint16_t lba, sector_t *sector) {
  for (int i = batch->count - 1; i >= 0; i--) {
    if (batch->lbas[i] == lba) {
      memcpy(sector->data, batch->data[i], SECTOR_SIZE);
      sector->valid = true;
      return true;
    }
  }
  return fat12_read_sector(batch->fat, lba, sector);
}

static fat12_err_t fat12_set_entry(fat12_write_batch_t *batch,
                                   uint16_t cluster, uint16_t value) {
  fat12_t *fat = batch->fat;

  uint32_t fat_offset = cluster + (cluster / 2);
  uint16_t fat_sector_lba = fat->fat_start_sector + (fat_offset / SECTOR_SIZE);
  uint16_t entry_offset = fat_offset % SECTOR_SIZE;

  sector_t sector;
  if (!fat12_read_sector_batched(batch, fat_sector_lba, &sector)) {
    return FAT12_ERR_READ;
  }

  if (entry_offset == SECTOR_SIZE - 1) {
    sector_t sector2;
    if (!fat12_read_sector_batched(batch, fat_sector_lba + 1, &sector2)) {
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

      fat12_err_t err = fat12_write_sector_batched(batch, lba1, sector.data);
      if (err != FAT12_OK) return err;

      err = fat12_write_sector_batched(batch, lba1 + 1, sector2.data);
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

      fat12_err_t err = fat12_write_sector_batched(batch, lba, sector.data);
      if (err != FAT12_OK) return err;
    }
  }

  return FAT12_OK;
}

static fat12_err_t fat12_get_entry_batched(fat12_write_batch_t *batch,
                                            uint16_t cluster, uint16_t *next) {
  fat12_t *fat = batch->fat;
  batched_read_ctx_t ctx = { .batch = batch };
  return fat12_resolve_entry(cluster, fat->total_clusters,
                              fat->fat_start_sector, fat->bpb.sectors_per_fat,
                              fat12_read_sector_batched_fn, &ctx, next);
}

static fat12_err_t fat12_find_free_cluster_from(fat12_write_batch_t *batch,
                                                 uint16_t start, uint16_t *out) {
  fat12_t *fat = batch->fat;
  if (start < 2) start = 2;
  for (uint16_t cluster = start; cluster < fat->total_clusters + 2; cluster++) {
    uint16_t entry;
    fat12_err_t err = fat12_get_entry_batched(batch, cluster, &entry);
    if (err != FAT12_OK) return err;

    if (fat12_is_free(entry)) {
      *out = cluster;
      return FAT12_OK;
    }
  }
  return FAT12_ERR_FULL;
}

static fat12_err_t fat12_write_cluster(fat12_write_batch_t *batch,
                                       uint16_t cluster, const uint8_t *buf) {
  fat12_t *fat = batch->fat;

  if (cluster < 2 || fat12_is_eof(cluster) || fat12_is_bad(cluster)) {
    return FAT12_ERR_INVALID;
  }

  if (cluster >= fat->total_clusters + 2) {
    return FAT12_ERR_INVALID;
  }

  uint16_t lba = fat12_cluster_to_lba(fat, cluster);

  for (uint8_t i = 0; i < fat->bpb.sectors_per_cluster; i++) {
    fat12_err_t err = fat12_write_sector_batched(batch, lba + i, buf + i * SECTOR_SIZE);
    if (err != FAT12_OK) return err;
  }

  return FAT12_OK;
}

static fat12_err_t fat12_write_root_entry(fat12_write_batch_t *batch,
                                           uint16_t index,
                                           fat12_dirent_t *entry) {
  fat12_t *fat = batch->fat;

  if (index >= fat->bpb.root_entries) {
    return FAT12_ERR_EOF;
  }

  uint16_t sector_lba = fat->root_dir_start_sector +
                        (index * FAT12_DIR_ENTRY_SIZE) / SECTOR_SIZE;
  uint16_t offset = (index * FAT12_DIR_ENTRY_SIZE) % SECTOR_SIZE;

  sector_t sector;
  if (!fat12_read_sector_batched(batch, sector_lba, &sector)) {
    return FAT12_ERR_READ;
  }

  memcpy(&sector.data[offset], entry, sizeof(*entry));

  return fat12_write_sector_batched(batch, sector_lba, sector.data);
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
  if (fat12_find(fat, filename, &existing) == FAT12_OK) {
    return FAT12_ERR_INVALID;
  }

  uint16_t dirent_idx;
  fat12_err_t err = fat12_find_free_dirent(fat, &dirent_idx);
  if (err != FAT12_OK) return err;

  memset(entry, 0, sizeof(*entry));
  fat12_format_name(filename, entry->name, entry->ext);
  entry->attr = FAT12_ATTR_ARCHIVE;
  entry->start_cluster = 0;
  entry->size = 0;

  if (fat->batch_in_use) return FAT12_ERR_INVALID;
  fat->batch_in_use = true;
  fat12_write_batch_init(&fat->batch, fat);

  err = fat12_write_root_entry(&fat->batch, dirent_idx, entry);
  if (err != FAT12_OK) { fat->batch_in_use = false; return err; }

  err = fat12_write_batch_flush(&fat->batch);
  fat->batch_in_use = false;
  return err;
}

static void fat12_init_dirent(fat12_dirent_t *d, const char *name8, const char *ext3) {
  memset(d, 0, sizeof(*d));
  memcpy(d->name, name8, 8);
  memcpy(d->ext, ext3, 3);
  d->attr = FAT12_ATTR_ARCHIVE;
}

fat12_err_t fat12_open_write(fat12_t *fat, const char *filename, fat12_writer_t *writer) {
  if (fat->batch_in_use) return FAT12_ERR_INVALID;

  memset(writer, 0, sizeof(*writer));
  writer->fat = fat;
  writer->batch = &fat->batch;
  fat->batch_in_use = true;
  fat12_write_batch_init(writer->batch, fat);

  char name8[8], ext3[3];
  fat12_format_name(filename, name8, ext3);

  for (uint16_t i = 0; i < fat->bpb.root_entries; i++) {
    fat12_err_t err = fat12_read_root_entry(fat, i, &writer->dirent);
    if (err != FAT12_OK) return err;

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

      uint16_t cluster = writer->dirent.start_cluster;
      while (cluster >= 2 && !fat12_is_eof(cluster) && !fat12_is_bad(cluster)) {
        uint16_t next;
        fat12_err_t err = fat12_get_entry(fat, cluster, &next);
        if (err != FAT12_OK) break;

        err = fat12_set_entry(writer->batch, cluster, 0);
        if (err != FAT12_OK) return err;

        cluster = next;
      }

      writer->dirent.start_cluster = 0;
      writer->dirent.size = 0;
      return FAT12_OK;
    }
  }

  return FAT12_ERR_FULL;
}

int fat12_write(fat12_writer_t *writer, const uint8_t *buf, uint16_t len) {
  fat12_t *fat = writer->fat;
  uint16_t cluster_size = fat->bpb.sectors_per_cluster * SECTOR_SIZE;
  uint16_t total_written = 0;

  if (cluster_size > FAT12_MAX_CLUSTER_SECTORS * SECTOR_SIZE) {
    return -FAT12_ERR_INVALID;
  }

  while (len > 0) {
    if (writer->current_cluster == 0 || writer->cluster_offset >= cluster_size) {
      uint16_t new_cluster;
      fat12_err_t err = fat12_find_free_cluster_from(writer->batch, writer->next_free_hint, &new_cluster);
      if (err != FAT12_OK) return -err;

      err = fat12_set_entry(writer->batch, new_cluster, 0xFFF);
      if (err != FAT12_OK) return -err;

      if (writer->prev_cluster != 0) {
        err = fat12_set_entry(writer->batch, writer->prev_cluster, new_cluster);
        if (err != FAT12_OK) return -err;
      }

      if (writer->first_cluster == 0) {
        writer->first_cluster = new_cluster;
      }

      writer->prev_cluster = writer->current_cluster;
      writer->current_cluster = new_cluster;
      writer->cluster_offset = 0;
      writer->next_free_hint = new_cluster + 1;
    }

    uint16_t remaining_in_cluster = cluster_size - writer->cluster_offset;
    uint16_t to_write = len;
    if (to_write > remaining_in_cluster) to_write = remaining_in_cluster;

    uint8_t cluster_buf[FAT12_MAX_CLUSTER_SECTORS * SECTOR_SIZE];
    if (writer->cluster_offset > 0) {
      uint16_t lba = fat12_cluster_to_lba(fat, writer->current_cluster);
      for (uint8_t i = 0; i < fat->bpb.sectors_per_cluster; i++) {
        sector_t sector;
        if (!fat12_read_sector_batched(writer->batch, lba + i, &sector)) {
          memset(cluster_buf + i * SECTOR_SIZE, 0, SECTOR_SIZE);
        } else {
          memcpy(cluster_buf + i * SECTOR_SIZE, sector.data, SECTOR_SIZE);
        }
      }
      memcpy(cluster_buf + writer->cluster_offset, buf, to_write);
    } else if (to_write < cluster_size) {
      memset(cluster_buf, 0, cluster_size);
      memcpy(cluster_buf, buf, to_write);
    } else {
      memcpy(cluster_buf, buf, cluster_size);
    }

    fat12_err_t err = fat12_write_cluster(writer->batch, writer->current_cluster, cluster_buf);
    if (err != FAT12_OK) return -err;

    buf += to_write;
    len -= to_write;
    writer->bytes_written += to_write;
    writer->cluster_offset += to_write;
    total_written += to_write;

    if (writer->cluster_offset >= cluster_size) {
      writer->prev_cluster = writer->current_cluster;
      writer->current_cluster = 0;
    }
  }

  return total_written;
}

fat12_err_t fat12_close_write(fat12_writer_t *writer) {
  writer->dirent.start_cluster = writer->first_cluster;
  writer->dirent.size = writer->bytes_written;

  fat12_err_t err = fat12_write_root_entry(writer->batch, writer->dirent_index, &writer->dirent);
  if (err != FAT12_OK) {
    writer->fat->batch_in_use = false;
    return err;
  }

  err = fat12_write_batch_flush(writer->batch);
  writer->fat->batch_in_use = false;
  return err;
}

fat12_err_t fat12_delete(fat12_t *fat, const char *filename) {
  if (fat->batch_in_use) return FAT12_ERR_INVALID;

  fat12_dirent_t entry;
  char name8[8], ext3[3];
  fat12_format_name(filename, name8, ext3);

  fat->batch_in_use = true;
  fat12_write_batch_init(&fat->batch, fat);

  for (uint16_t i = 0; i < fat->bpb.root_entries; i++) {
    fat12_err_t err = fat12_read_root_entry(fat, i, &entry);
    if (err != FAT12_OK) { fat->batch_in_use = false; return err; }

    if (fat12_entry_is_end(&entry)) {
      fat->batch_in_use = false;
      return FAT12_ERR_NOT_FOUND;
    }

    if (!fat12_entry_valid(&entry)) continue;

    if (memcmp(entry.name, name8, 8) == 0 &&
        memcmp(entry.ext, ext3, 3) == 0) {
      uint16_t cluster = entry.start_cluster;
      while (cluster >= 2 && !fat12_is_eof(cluster) && !fat12_is_bad(cluster)) {
        uint16_t next;
        err = fat12_get_entry(fat, cluster, &next);
        if (err != FAT12_OK) break;

        err = fat12_set_entry(&fat->batch, cluster, 0);
        if (err != FAT12_OK) { fat->batch_in_use = false; return err; }

        cluster = next;
      }

      entry.name[0] = FAT12_DIRENT_FREE;
      err = fat12_write_root_entry(&fat->batch, i, &entry);
      if (err != FAT12_OK) { fat->batch_in_use = false; return err; }

      err = fat12_write_batch_flush(&fat->batch);
      fat->batch_in_use = false;
      return err;
    }
  }

  fat->batch_in_use = false;
  return FAT12_ERR_NOT_FOUND;
}

static void fat12_build_boot_sector(uint8_t *boot, const fat12_bpb_t *bpb,
                                    const char *volume_label) {
  memset(boot, 0, SECTOR_SIZE);

  boot[0] = 0xEB; boot[1] = 0x3C; boot[2] = 0x90;
  memcpy(&boot[3], "MSDOS5.0", 8);

  fat12_bpb_raw_t raw;
  fat12_bpb_to_raw(&raw, bpb);
  memcpy(&boot[FAT12_BPB_OFFSET], &raw, sizeof(raw));

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

static void fat12_init_hd_layout(fat12_layout_t *lay) {
  memset(lay, 0, sizeof(*lay));
  lay->bpb.bytes_per_sector    = SECTOR_SIZE;
  lay->bpb.sectors_per_cluster = 1;
  lay->bpb.reserved_sectors    = 1;
  lay->bpb.num_fats            = 2;
  lay->bpb.root_entries        = 224;
  lay->bpb.total_sectors       = 80 * 2 * 18;
  lay->bpb.media_descriptor    = 0xF0;
  lay->bpb.sectors_per_fat     = 9;
  lay->bpb.sectors_per_track   = 18;
  lay->bpb.num_heads           = 2;

  lay->fat_start_sector = lay->bpb.reserved_sectors;
  lay->root_dir_start_sector = lay->fat_start_sector +
                               (lay->bpb.num_fats * lay->bpb.sectors_per_fat);
  uint16_t root_dir_sectors = (lay->bpb.root_entries * FAT12_DIR_ENTRY_SIZE +
                               SECTOR_SIZE - 1) / SECTOR_SIZE;
  lay->root_dir_sectors = root_dir_sectors;
  lay->data_start_sector = lay->root_dir_start_sector + root_dir_sectors;
}

static void fat12_fill_format_sector(sector_t *s, uint16_t lba,
                                     const fat12_layout_t *lay,
                                     const uint8_t *boot,
                                     const uint8_t *fat_sector,
                                     const uint8_t *root_first,
                                     const char *volume_label,
                                     bool write_all_tracks) {
  uint16_t fat2_start = lay->fat_start_sector + lay->bpb.sectors_per_fat;

  if (lba == 0) {
    memcpy(s->data, boot, SECTOR_SIZE);
  } else if (lba >= lay->fat_start_sector && lba < fat2_start) {
    if (lba == lay->fat_start_sector)
      memcpy(s->data, fat_sector, SECTOR_SIZE);
    else
      memset(s->data, 0, SECTOR_SIZE);
  } else if (lba >= fat2_start && lba < fat2_start + lay->bpb.sectors_per_fat) {
    if (lba == fat2_start)
      memcpy(s->data, fat_sector, SECTOR_SIZE);
    else
      memset(s->data, 0, SECTOR_SIZE);
  } else if (lba >= lay->root_dir_start_sector &&
             lba < lay->root_dir_start_sector + lay->root_dir_sectors) {
    if (lba == lay->root_dir_start_sector && volume_label)
      memcpy(s->data, root_first, SECTOR_SIZE);
    else
      memset(s->data, 0, SECTOR_SIZE);
  } else {
    memset(s->data, 0, SECTOR_SIZE);
    if (!write_all_tracks && lba >= lay->data_start_sector)
      s->valid = false;
  }
}

fat12_err_t fat12_format(fat12_io_t io, const char *volume_label, bool write_all_tracks) {
  if (io.write == NULL)
    return FAT12_ERR_INVALID;

  fat12_layout_t lay;
  fat12_init_hd_layout(&lay);

  uint8_t boot[SECTOR_SIZE];
  fat12_build_boot_sector(boot, &lay.bpb, volume_label);

  uint8_t fat_sector[SECTOR_SIZE];
  memset(fat_sector, 0, sizeof(fat_sector));
  fat_sector[0] = lay.bpb.media_descriptor;
  fat_sector[1] = 0xFF;
  fat_sector[2] = 0xFF;

  uint8_t root_first[SECTOR_SIZE];
  fat12_build_volume_label(root_first, volume_label);

  for (uint8_t cyl = 0; cyl < 80; cyl++) {
    for (uint8_t side = 0; side < lay.bpb.num_heads; side++) {
      track_t t;
      memset(&t, 0, sizeof(t));
      t.track = cyl;
      t.side = side;

      bool has_valid = false;
      for (uint8_t s = 0; s < lay.bpb.sectors_per_track; s++) {
        uint16_t lba = (t.track * lay.bpb.num_heads + side) *
                       lay.bpb.sectors_per_track + s;

        t.sectors[s].track = t.track;
        t.sectors[s].side = side;
        t.sectors[s].sector_n = s + 1;
        t.sectors[s].size_code = 2;
        t.sectors[s].valid = true;

        fat12_fill_format_sector(&t.sectors[s], lba, &lay, boot,
                                 fat_sector, root_first, volume_label,
                                 write_all_tracks);

        if (t.sectors[s].valid) has_valid = true;
      }

      if (has_valid) {
        if (!io.write(io.ctx, &t))
          return FAT12_ERR_WRITE;
      }

      if (!write_all_tracks) {
        uint16_t track_end_lba = (t.track * lay.bpb.num_heads + side + 1) *
                                 lay.bpb.sectors_per_track;
        if (track_end_lba > lay.data_start_sector)
          return FAT12_OK;
      }
    }
  }

  return FAT12_OK;
}
