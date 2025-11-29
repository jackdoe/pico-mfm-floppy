#include "fat12.h"
#include <string.h>

// ============== CHS Conversion ==============

void fat12_lba_to_chs(fat12_t *fat, uint16_t lba, uint8_t *c, uint8_t *h, uint8_t *s) {
  *c = lba / (fat->bpb.num_heads * fat->bpb.sectors_per_track);
  uint16_t temp = lba % (fat->bpb.num_heads * fat->bpb.sectors_per_track);
  *h = temp / fat->bpb.sectors_per_track;
  *s = (temp % fat->bpb.sectors_per_track) + 1;  // Sectors are 1-based
}

// ============== Internal: Read sector by LBA ==============

bool fat12_read_sector(fat12_t *fat, uint16_t lba, sector_t *sector) {
  uint8_t c, h, s;
  fat12_lba_to_chs(fat, lba, &c, &h, &s);
  sector->track = c;
  sector->side = h;
  sector->sector_n = s;
  sector->valid = false;
  return fat->io.read(fat->io.ctx, sector);
}

// ============== Initialize FAT12 ==============

fat12_err_t fat12_init(fat12_t *fat, fat12_io_t io) {
  memset(fat, 0, sizeof(*fat));
  fat->io = io;

  // Validate callbacks
  if (io.read == NULL) {
    return FAT12_ERR_INVALID;
  }

  // Read boot sector directly (LBA 0 = C0 H0 S1)
  // Can't use fat12_read_sector yet because BPB isn't parsed
  fat->sector_buf.track = 0;
  fat->sector_buf.side = 0;
  fat->sector_buf.sector_n = 1;
  fat->sector_buf.valid = false;
  if (!fat->io.read(fat->io.ctx, &fat->sector_buf)) {
    return FAT12_ERR_READ;
  }

  // Check boot signature
  uint8_t *b = fat->sector_buf.data;
  if (b[510] != 0x55 || b[511] != 0xAA) {
    return FAT12_ERR_INVALID;
  }

  // Parse BPB (BIOS Parameter Block)
  fat->bpb.bytes_per_sector    = b[11] | (b[12] << 8);
  fat->bpb.sectors_per_cluster = b[13];
  fat->bpb.reserved_sectors    = b[14] | (b[15] << 8);
  fat->bpb.num_fats            = b[16];
  fat->bpb.root_entries        = b[17] | (b[18] << 8);
  fat->bpb.total_sectors       = b[19] | (b[20] << 8);
  fat->bpb.media_descriptor    = b[21];
  fat->bpb.sectors_per_fat     = b[22] | (b[23] << 8);
  fat->bpb.sectors_per_track   = b[24] | (b[25] << 8);
  fat->bpb.num_heads           = b[26] | (b[27] << 8);
  fat->bpb.hidden_sectors      = b[28] | (b[29] << 8) | (b[30] << 16) | (b[31] << 24);

  // Validate basic params
  if (fat->bpb.bytes_per_sector != SECTOR_SIZE ||
      fat->bpb.sectors_per_cluster == 0 ||
      fat->bpb.sectors_per_cluster > 64 ||  // Max 64 sectors (32KB) per cluster
      fat->bpb.num_fats == 0 ||
      fat->bpb.sectors_per_track == 0 ||
      fat->bpb.num_heads == 0) {
    return FAT12_ERR_INVALID;
  }

  // Compute layout
  fat->fat_start_sector = fat->bpb.reserved_sectors;
  fat->root_dir_start_sector = fat->fat_start_sector +
                                (fat->bpb.num_fats * fat->bpb.sectors_per_fat);
  fat->root_dir_sectors = (fat->bpb.root_entries * FAT12_DIR_ENTRY_SIZE +
                           SECTOR_SIZE - 1) / SECTOR_SIZE;
  fat->data_start_sector = fat->root_dir_start_sector + fat->root_dir_sectors;
  fat->total_clusters = (fat->bpb.total_sectors - fat->data_start_sector) /
                         fat->bpb.sectors_per_cluster;

  return FAT12_OK;
}

// ============== Get FAT entry (12-bit) ==============

fat12_err_t fat12_get_entry(fat12_t *fat, uint16_t cluster, uint16_t *next) {
  // Validate cluster is within FAT range
  uint32_t max_cluster = fat->total_clusters + 2;
  if (cluster >= max_cluster && cluster < 0xFF0) {
    *next = 0;
    return FAT12_ERR_INVALID;
  }

  // FAT12: 1.5 bytes per entry
  uint32_t fat_offset = cluster + (cluster / 2);

  // Validate fat_offset is within FAT area
  uint32_t fat_size_bytes = (uint32_t)fat->bpb.sectors_per_fat * SECTOR_SIZE;
  if (fat_offset + 1 >= fat_size_bytes) {
    *next = 0;
    return FAT12_ERR_INVALID;
  }

  uint16_t fat_sector = fat->fat_start_sector + (fat_offset / SECTOR_SIZE);
  uint16_t entry_offset = fat_offset % SECTOR_SIZE;

  // Read FAT sector
  if (!fat12_read_sector(fat, fat_sector, &fat->sector_buf)) {
    return FAT12_ERR_READ;
  }

  uint16_t value;
  if (entry_offset == SECTOR_SIZE - 1) {
    // Entry spans two sectors
    value = fat->sector_buf.data[entry_offset];
    if (!fat12_read_sector(fat, fat_sector + 1, &fat->sector_buf)) {
      return FAT12_ERR_READ;
    }
    value |= fat->sector_buf.data[0] << 8;
  } else {
    value = fat->sector_buf.data[entry_offset] | (fat->sector_buf.data[entry_offset + 1] << 8);
  }

  // Extract 12-bit value
  if (cluster & 1) {
    *next = value >> 4;
  } else {
    *next = value & 0x0FFF;
  }

  return FAT12_OK;
}

// ============== Cluster status checks ==============

bool fat12_is_eof(uint16_t cluster) {
  return cluster >= 0xFF8;
}

bool fat12_is_free(uint16_t cluster) {
  return cluster == 0;
}

bool fat12_is_bad(uint16_t cluster) {
  return cluster == 0xFF7;
}

// ============== Convert cluster to first LBA ==============

uint16_t fat12_cluster_to_lba(fat12_t *fat, uint16_t cluster) {
  return fat->data_start_sector + (cluster - 2) * fat->bpb.sectors_per_cluster;
}

// ============== Read root directory entry ==============

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

// ============== Directory entry checks ==============

bool fat12_entry_valid(fat12_dirent_t *entry) {
  uint8_t first = (uint8_t)entry->name[0];
  if (first == 0x00 || first == 0xE5) return false;  // Empty or deleted
  if (entry->attr == FAT12_ATTR_LFN) return false;   // Long filename entry
  return true;
}

bool fat12_entry_is_end(fat12_dirent_t *entry) {
  return (uint8_t)entry->name[0] == 0x00;
}

// ============== Format filename for comparison ==============

void fat12_format_name(const char *input, char *name8, char *ext3) {
  memset(name8, ' ', 8);
  memset(ext3, ' ', 3);

  int i = 0;
  // Copy name part (before dot or end)
  while (*input && *input != '.' && i < 8) {
    name8[i++] = (*input >= 'a' && *input <= 'z') ? (*input - 32) : *input;
    input++;
  }

  // Skip dot if present
  if (*input == '.') input++;

  // Copy extension
  i = 0;
  while (*input && i < 3) {
    ext3[i++] = (*input >= 'a' && *input <= 'z') ? (*input - 32) : *input;
    input++;
  }
}

// ============== Find file in root directory ==============

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

// ============== Read file cluster ==============

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

// ============== Open file for reading ==============

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

// ============== Read from file ==============

int fat12_read(fat12_file_t *file, uint8_t *buf, uint16_t len) {
  fat12_t *fat = file->fat;
  uint16_t cluster_size = fat->bpb.sectors_per_cluster * SECTOR_SIZE;
  uint16_t total_read = 0;

  if (cluster_size > 64 * SECTOR_SIZE) {
    return -FAT12_ERR_INVALID;
  }

  while (len > 0 && file->bytes_read < file->file_size) {
    if (file->current_cluster < 2 || fat12_is_eof(file->current_cluster)) {
      break;
    }

    uint8_t cluster_buf[64 * SECTOR_SIZE];
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

// ============== Write Batch Operations ==============

void fat12_write_batch_init(fat12_write_batch_t *batch, fat12_t *fat) {
  batch->fat = fat;
  batch->count = 0;
}

fat12_err_t fat12_write_batch_add(fat12_write_batch_t *batch, uint16_t lba, const uint8_t *data) {
  if (batch->count >= FAT12_WRITE_BATCH_MAX) {
    return FAT12_ERR_FULL;
  }
  batch->lbas[batch->count] = lba;
  memcpy(batch->data[batch->count], data, SECTOR_SIZE);
  batch->count++;
  return FAT12_OK;
}

fat12_err_t fat12_write_batch_flush(fat12_write_batch_t *batch) {
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
        track.sectors[idx].size = SECTOR_SIZE;
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

// ============== Internal: Write sector batched ==============

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

// ============== Read sector from batch or disk ==============

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

// ============== Set FAT entry (12-bit) ==============

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

// ============== Find free cluster ==============

static fat12_err_t fat12_find_free_cluster_from(fat12_t *fat, uint16_t start, uint16_t *out) {
  if (start < 2) start = 2;
  for (uint16_t cluster = start; cluster < fat->total_clusters + 2; cluster++) {
    uint16_t entry;
    fat12_err_t err = fat12_get_entry(fat, cluster, &entry);
    if (err != FAT12_OK) return err;

    if (fat12_is_free(entry)) {
      *out = cluster;
      return FAT12_OK;
    }
  }
  return FAT12_ERR_FULL;
}

// ============== Write cluster ==============

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

// ============== Write root directory entry ==============

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
  if (!fat12_read_sector(fat, sector_lba, &sector)) {
    return FAT12_ERR_READ;
  }

  memcpy(&sector.data[offset], entry, sizeof(*entry));

  return fat12_write_sector_batched(batch, sector_lba, sector.data);
}

// ============== Find free directory entry ==============

static fat12_err_t fat12_find_free_dirent(fat12_t *fat, uint16_t *index) {
  fat12_dirent_t entry;

  for (uint16_t i = 0; i < fat->bpb.root_entries; i++) {
    fat12_err_t err = fat12_read_root_entry(fat, i, &entry);
    if (err != FAT12_OK) return err;

    uint8_t first = (uint8_t)entry.name[0];
    if (first == 0x00 || first == 0xE5) {
      *index = i;
      return FAT12_OK;
    }
  }
  return FAT12_ERR_FULL;
}

// ============== Create new file ==============

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

  fat12_write_batch_t batch;
  fat12_write_batch_init(&batch, fat);

  err = fat12_write_root_entry(&batch, dirent_idx, entry);
  if (err != FAT12_OK) return err;

  return fat12_write_batch_flush(&batch);
}

// ============== Open file for writing ==============

fat12_err_t fat12_open_write(fat12_t *fat, const char *filename, fat12_writer_t *writer) {
  memset(writer, 0, sizeof(*writer));
  writer->fat = fat;
  fat12_write_batch_init(&writer->batch, fat);

  char name8[8], ext3[3];
  fat12_format_name(filename, name8, ext3);

  for (uint16_t i = 0; i < fat->bpb.root_entries; i++) {
    fat12_err_t err = fat12_read_root_entry(fat, i, &writer->dirent);
    if (err != FAT12_OK) return err;

    if (fat12_entry_is_end(&writer->dirent)) {
      writer->dirent_index = i;
      memset(&writer->dirent, 0, sizeof(writer->dirent));
      memcpy(writer->dirent.name, name8, 8);
      memcpy(writer->dirent.ext, ext3, 3);
      writer->dirent.attr = FAT12_ATTR_ARCHIVE;
      return FAT12_OK;
    }

    uint8_t first = (uint8_t)writer->dirent.name[0];
    if (first == 0xE5) {
      writer->dirent_index = i;
      memset(&writer->dirent, 0, sizeof(writer->dirent));
      memcpy(writer->dirent.name, name8, 8);
      memcpy(writer->dirent.ext, ext3, 3);
      writer->dirent.attr = FAT12_ATTR_ARCHIVE;
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

        err = fat12_set_entry(&writer->batch, cluster, 0);
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

// ============== Write to file ==============

int fat12_write(fat12_writer_t *writer, const uint8_t *buf, uint16_t len) {
  fat12_t *fat = writer->fat;
  uint16_t cluster_size = fat->bpb.sectors_per_cluster * SECTOR_SIZE;
  uint16_t total_written = 0;

  if (cluster_size > 64 * SECTOR_SIZE) {
    return -FAT12_ERR_INVALID;
  }

  while (len > 0) {
    if (writer->current_cluster == 0 || writer->cluster_offset >= cluster_size) {
      uint16_t new_cluster;
      fat12_err_t err = fat12_find_free_cluster_from(fat, writer->next_free_hint, &new_cluster);
      if (err != FAT12_OK) return -err;

      err = fat12_set_entry(&writer->batch, new_cluster, 0xFFF);
      if (err != FAT12_OK) return -err;

      if (writer->prev_cluster != 0) {
        err = fat12_set_entry(&writer->batch, writer->prev_cluster, new_cluster);
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

    uint8_t cluster_buf[64 * SECTOR_SIZE];
    if (writer->cluster_offset > 0) {
      uint16_t lba = fat12_cluster_to_lba(fat, writer->current_cluster);
      for (uint8_t i = 0; i < fat->bpb.sectors_per_cluster; i++) {
        sector_t sector;
        if (!fat12_read_sector(fat, lba + i, &sector)) {
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

    fat12_err_t err = fat12_write_cluster(&writer->batch, writer->current_cluster, cluster_buf);
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

// ============== Close file ==============

fat12_err_t fat12_close_write(fat12_writer_t *writer) {
  writer->dirent.start_cluster = writer->first_cluster;
  writer->dirent.size = writer->bytes_written;

  fat12_err_t err = fat12_write_root_entry(&writer->batch, writer->dirent_index, &writer->dirent);
  if (err != FAT12_OK) return err;

  return fat12_write_batch_flush(&writer->batch);
}

// ============== Delete file ==============

fat12_err_t fat12_delete(fat12_t *fat, const char *filename) {
  fat12_dirent_t entry;
  char name8[8], ext3[3];
  fat12_format_name(filename, name8, ext3);

  fat12_write_batch_t batch;
  fat12_write_batch_init(&batch, fat);

  for (uint16_t i = 0; i < fat->bpb.root_entries; i++) {
    fat12_err_t err = fat12_read_root_entry(fat, i, &entry);
    if (err != FAT12_OK) return err;

    if (fat12_entry_is_end(&entry)) {
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

        err = fat12_set_entry(&batch, cluster, 0);
        if (err != FAT12_OK) return err;

        cluster = next;
      }

      entry.name[0] = 0xE5;
      err = fat12_write_root_entry(&batch, i, &entry);
      if (err != FAT12_OK) return err;

      return fat12_write_batch_flush(&batch);
    }
  }

  return FAT12_ERR_NOT_FOUND;
}

// ============== Format disk ==============

fat12_err_t fat12_format(fat12_io_t io, const char *volume_label, bool write_all_tracks) {
  if (io.write == NULL) {
    return FAT12_ERR_INVALID;
  }

  const uint8_t num_tracks = 80;
  const uint8_t num_heads = 2;
  const uint8_t sectors_per_track = 18;
  const uint16_t total_sectors = num_tracks * num_heads * sectors_per_track;
  const uint8_t sectors_per_cluster = 1;
  const uint16_t reserved_sectors = 1;
  const uint8_t num_fats = 2;
  const uint16_t root_entries = 224;
  const uint8_t media_descriptor = 0xF0;
  const uint16_t sectors_per_fat = 9;

  fat12_t fat;
  memset(&fat, 0, sizeof(fat));
  fat.io = io;
  fat.bpb.bytes_per_sector = SECTOR_SIZE;
  fat.bpb.sectors_per_cluster = sectors_per_cluster;
  fat.bpb.reserved_sectors = reserved_sectors;
  fat.bpb.num_fats = num_fats;
  fat.bpb.root_entries = root_entries;
  fat.bpb.total_sectors = total_sectors;
  fat.bpb.media_descriptor = media_descriptor;
  fat.bpb.sectors_per_fat = sectors_per_fat;
  fat.bpb.sectors_per_track = sectors_per_track;
  fat.bpb.num_heads = num_heads;
  fat.bpb.hidden_sectors = 0;

  fat.fat_start_sector = reserved_sectors;
  fat.root_dir_start_sector = fat.fat_start_sector + (num_fats * sectors_per_fat);
  fat.root_dir_sectors = (root_entries * FAT12_DIR_ENTRY_SIZE + SECTOR_SIZE - 1) / SECTOR_SIZE;
  fat.data_start_sector = fat.root_dir_start_sector + fat.root_dir_sectors;
  fat.total_clusters = (total_sectors - fat.data_start_sector) / sectors_per_cluster;

  // Build boot sector
  uint8_t boot[SECTOR_SIZE];
  memset(boot, 0, sizeof(boot));

  boot[0] = 0xEB;
  boot[1] = 0x3C;
  boot[2] = 0x90;

  memcpy(&boot[3], "MSDOS5.0", 8);

  boot[11] = SECTOR_SIZE & 0xFF;
  boot[12] = SECTOR_SIZE >> 8;
  boot[13] = sectors_per_cluster;
  boot[14] = reserved_sectors & 0xFF;
  boot[15] = reserved_sectors >> 8;
  boot[16] = num_fats;
  boot[17] = root_entries & 0xFF;
  boot[18] = root_entries >> 8;
  boot[19] = total_sectors & 0xFF;
  boot[20] = total_sectors >> 8;
  boot[21] = media_descriptor;
  boot[22] = sectors_per_fat & 0xFF;
  boot[23] = sectors_per_fat >> 8;
  boot[24] = sectors_per_track & 0xFF;
  boot[25] = sectors_per_track >> 8;
  boot[26] = num_heads & 0xFF;
  boot[27] = num_heads >> 8;
  boot[28] = 0; boot[29] = 0; boot[30] = 0; boot[31] = 0;

  boot[36] = 0x00;
  boot[37] = 0x00;
  boot[38] = 0x29;
  boot[39] = 0x12; boot[40] = 0x34; boot[41] = 0x56; boot[42] = 0x78;

  if (volume_label) {
    int i;
    for (i = 0; i < 11 && volume_label[i]; i++) {
      char c = volume_label[i];
      boot[43 + i] = (c >= 'a' && c <= 'z') ? (c - 32) : c;
    }
    for (; i < 11; i++) {
      boot[43 + i] = ' ';
    }
  } else {
    memcpy(&boot[43], "NO NAME    ", 11);
  }

  memcpy(&boot[54], "FAT12   ", 8);

  boot[510] = 0x55;
  boot[511] = 0xAA;

  uint8_t fat_sector[SECTOR_SIZE];
  memset(fat_sector, 0, sizeof(fat_sector));
  fat_sector[0] = media_descriptor;
  fat_sector[1] = 0xFF;
  fat_sector[2] = 0xFF;

  uint8_t root_first_sector[SECTOR_SIZE];
  memset(root_first_sector, 0, sizeof(root_first_sector));
  if (volume_label) {
    fat12_dirent_t *label_entry = (fat12_dirent_t *)root_first_sector;
    int i;
    for (i = 0; i < 8 && volume_label[i] && volume_label[i] != '.'; i++) {
      char c = volume_label[i];
      label_entry->name[i] = (c >= 'a' && c <= 'z') ? (c - 32) : c;
    }
    for (; i < 8; i++) {
      label_entry->name[i] = ' ';
    }
    int j = 0;
    int label_len = 0;
    while (volume_label[label_len]) label_len++;
    if (label_len > 8) {
      for (j = 0; j < 3 && (8 + j) < label_len; j++) {
        char c = volume_label[8 + j];
        label_entry->ext[j] = (c >= 'a' && c <= 'z') ? (c - 32) : c;
      }
    }
    for (; j < 3; j++) {
      label_entry->ext[j] = ' ';
    }
    label_entry->attr = FAT12_ATTR_VOLUME_ID;
  }

  for (uint8_t track = 0; track < num_tracks; track++) {
    for (uint8_t side = 0; side < num_heads; side++) {
      track_t t;
      memset(&t, 0, sizeof(t));
      t.track = track;
      t.side = side;

      for (uint8_t s = 0; s < sectors_per_track; s++) {
        uint16_t lba = (track * num_heads + side) * sectors_per_track + s;

        t.sectors[s].track = track;
        t.sectors[s].side = side;
        t.sectors[s].sector_n = s + 1;
        t.sectors[s].size_code = 2;
        t.sectors[s].size = SECTOR_SIZE;
        t.sectors[s].valid = true;

        if (lba == 0) {
          memcpy(t.sectors[s].data, boot, SECTOR_SIZE);
        } else if (lba >= fat.fat_start_sector && lba < fat.fat_start_sector + sectors_per_fat) {
          if (lba == fat.fat_start_sector) {
            memcpy(t.sectors[s].data, fat_sector, SECTOR_SIZE);
          } else {
            memset(t.sectors[s].data, 0, SECTOR_SIZE);
          }
        } else if (lba >= fat.fat_start_sector + sectors_per_fat &&
                   lba < fat.fat_start_sector + 2 * sectors_per_fat) {
          if (lba == fat.fat_start_sector + sectors_per_fat) {
            memcpy(t.sectors[s].data, fat_sector, SECTOR_SIZE);
          } else {
            memset(t.sectors[s].data, 0, SECTOR_SIZE);
          }
        } else if (lba >= fat.root_dir_start_sector &&
                   lba < fat.root_dir_start_sector + fat.root_dir_sectors) {
          if (lba == fat.root_dir_start_sector && volume_label) {
            memcpy(t.sectors[s].data, root_first_sector, SECTOR_SIZE);
          } else {
            memset(t.sectors[s].data, 0, SECTOR_SIZE);
          }
        } else {
          memset(t.sectors[s].data, 0, SECTOR_SIZE);
          if (!write_all_tracks && lba >= fat.data_start_sector) {
            t.sectors[s].valid = false;
          }
        }
      }

      bool has_valid = false;
      for (uint8_t s = 0; s < sectors_per_track; s++) {
        if (t.sectors[s].valid) {
          has_valid = true;
          break;
        }
      }

      if (has_valid) {
        if (!io.write(io.ctx, &t)) {
          return FAT12_ERR_WRITE;
        }
      }

      if (!write_all_tracks) {
        uint16_t track_end_lba = (track * num_heads + side + 1) * sectors_per_track;
        if (track_end_lba > fat.data_start_sector) {
          return FAT12_OK;
        }
      }
    }
  }

  return FAT12_OK;
}
