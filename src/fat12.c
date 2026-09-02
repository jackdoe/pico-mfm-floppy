#include "fat12.h"
#include <string.h>

#define FAT12_EOC 0x0FFFu
#define FAT12_BAD 0x0FF7u

typedef enum {
  FSCK_REPORT,
  FSCK_REPAIR_FAT,
  FSCK_REPAIR_DIR,
} fsck_mode_t;

typedef enum {
  LINK_NEXT,
  LINK_END,
  LINK_BAD,
  LINK_BROKEN,
  LINK_LOOP,
  LINK_CROSS,
} fsck_link_t;

typedef struct {
  fat12_t *fat;
  fat12_fsck_t *out;
  fsck_mode_t mode;
  uint16_t next_owner;
  uint16_t freed;
  uint16_t truncated;
  uint16_t removed;
  uint16_t duplicates_removed;
  uint16_t tail_cuts;
  uint16_t fat_start;
} fsck_ctx_t;

typedef struct {
  uint16_t lba;
  uint16_t offset;
} fsck_slot_t;

typedef struct {
  fat12_t *fat;
  uint16_t cluster;
  uint16_t cached_lba;
  uint16_t cached_lba2;
  uint16_t fat_start;
  uint8_t sector[DISK_SECTOR_SIZE];
  uint8_t sector2[DISK_SECTOR_SIZE];
} fat12_scan_t;

typedef struct {
  bool mismatch;
  bool valid[FAT12_NUM_FATS];
} fat12_mirrors_t;

typedef struct {
  bool found;
  uint16_t index;
  fat12_dirent_t entry;
  uint16_t free_index;
  bool free_is_end;
} fat12_lookup_t;

static disk_err_t fsck_compare_fats(fat12_t *fat, fat12_mirrors_t *mirrors);

static disk_err_t fsck_select_fat(fat12_t *fat,
                                  const fat12_mirrors_t *mirrors,
                                  fat12_fsck_t *out,
                                  uint16_t *fat_start);

static uint16_t load_le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t load_le32(const uint8_t *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void store_le16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

static void store_le32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

static bool fat12_bpb_valid(const uint8_t *boot) {
  const uint8_t *p = boot + FAT12_BPB_OFFSET;
  uint32_t fat_entries =
      (FAT12_SECTORS_PER_FAT * DISK_SECTOR_SIZE * 2u) / 3u;
  return load_le16(p + 0) == DISK_SECTOR_SIZE &&
         p[2] == FAT12_SECTORS_PER_CLUSTER &&
         load_le16(p + 3) == FAT12_RESERVED_SECTORS &&
         p[5] == FAT12_NUM_FATS &&
         load_le16(p + 6) == FAT12_ROOT_ENTRIES &&
         load_le16(p + 8) == DISK_SECTOR_COUNT &&
         p[10] == FAT12_MEDIA_DESCRIPTOR &&
         load_le16(p + 11) == FAT12_SECTORS_PER_FAT &&
         load_le16(p + 13) == DISK_SECTORS_PER_TRACK &&
         load_le16(p + 15) == DISK_HEADS &&
         load_le32(p + 17) == 0 &&
         load_le32(p + 21) == 0 &&
         FAT12_CLUSTER_LIMIT <= fat_entries;
}

static void fat12_encode_bpb(uint8_t *boot) {
  uint8_t *p = boot + FAT12_BPB_OFFSET;
  store_le16(p + 0, DISK_SECTOR_SIZE);
  p[2] = FAT12_SECTORS_PER_CLUSTER;
  store_le16(p + 3, FAT12_RESERVED_SECTORS);
  p[5] = FAT12_NUM_FATS;
  store_le16(p + 6, FAT12_ROOT_ENTRIES);
  store_le16(p + 8, DISK_SECTOR_COUNT);
  p[10] = FAT12_MEDIA_DESCRIPTOR;
  store_le16(p + 11, FAT12_SECTORS_PER_FAT);
  store_le16(p + 13, DISK_SECTORS_PER_TRACK);
  store_le16(p + 15, DISK_HEADS);
  store_le32(p + 17, 0);
  store_le32(p + 21, 0);
}

static void fat12_decode_dirent(fat12_dirent_t *entry, const uint8_t *raw) {
  memcpy(entry->name, raw, FAT12_FILENAME_LEN);
  memcpy(entry->ext, raw + 8, FAT12_EXTENSION_LEN);
  entry->attr = raw[11];
  memcpy(entry->reserved, raw + 12, sizeof(entry->reserved));
  entry->time = load_le16(raw + 22);
  entry->date = load_le16(raw + 24);
  entry->start_cluster = load_le16(raw + 26);
  entry->size = load_le32(raw + 28);
}

static void fat12_encode_dirent(uint8_t *raw, const fat12_dirent_t *entry) {
  memcpy(raw, entry->name, FAT12_FILENAME_LEN);
  memcpy(raw + 8, entry->ext, FAT12_EXTENSION_LEN);
  raw[11] = entry->attr;
  memcpy(raw + 12, entry->reserved, sizeof(entry->reserved));
  store_le16(raw + 22, entry->time);
  store_le16(raw + 24, entry->date);
  store_le16(raw + 26, entry->start_cluster);
  store_le32(raw + 28, entry->size);
}

static disk_err_t fat12_read_sector(fat12_t *fat, uint16_t lba,
                                    uint8_t out[static DISK_SECTOR_SIZE]) {
  return cache_read(fat->cache, lba, out);
}

static disk_err_t fat12_write_sector(fat12_t *fat, uint16_t lba,
                                     const uint8_t *data) {
  return cache_write(fat->cache, lba, data);
}

static disk_err_t fat12_flush(fat12_t *fat) {
  return cache_flush(fat->cache);
}

bool fat12_busy(const fat12_t *fat) {
  return fat != NULL && fat->transaction;
}

static disk_err_t fat12_mutable(fat12_t *fat) {
  disk_err_t err = cache_writable(fat->cache);
  if (err != DISK_OK) return err;
  return fat->transaction ? DISK_ERR_BUSY : DISK_OK;
}

static void fat12_end(fat12_t *fat) {
  cache_discard(fat->cache);
  fat->transaction = false;
}

disk_err_t fat12_init(fat12_t *fat, cache_t *cache) {
  if (fat == NULL || cache == NULL) return DISK_ERR_INVALID;
  memset(fat, 0, sizeof(*fat));
  fat->cache = cache;
  fat->fat_start = FAT12_FAT1_START;

  uint8_t boot[DISK_SECTOR_SIZE];
  disk_err_t err = fat12_read_sector(fat, 0, boot);
  if (err != DISK_OK) return err;
  if (boot[FAT12_BOOT_SIG_OFFSET] != 0x55 ||
      boot[FAT12_BOOT_SIG_OFFSET + 1] != 0xAA) {
    return DISK_ERR_INVALID;
  }
  if (!fat12_bpb_valid(boot)) return DISK_ERR_INVALID;

  fat12_mirrors_t mirrors;
  err = fsck_compare_fats(fat, &mirrors);
  if (err != DISK_OK) return err;
  if (mirrors.mismatch) {
    fat12_fsck_t selection;
    err = fsck_select_fat(fat, &mirrors, &selection, &fat->fat_start);
    if (err != DISK_OK) return err;
  }
  return DISK_OK;
}

bool fat12_is_eof(uint16_t cluster) {
  return cluster >= 0x0FF8u && cluster <= 0x0FFFu;
}

static bool fat12_is_bad(uint16_t cluster) {
  return cluster == FAT12_BAD;
}

static bool fat12_cluster_valid(uint16_t cluster) {
  return cluster >= 2 && cluster < FAT12_CLUSTER_LIMIT;
}

static uint16_t fat12_cluster_to_lba(uint16_t cluster) {
  return FAT12_DATA_START + cluster - 2u;
}

static disk_err_t fat12_resolve_entry(fat12_t *fat, uint16_t fat_start,
                                      uint16_t cluster, uint16_t *next) {
  if (next == NULL || !fat12_cluster_valid(cluster)) {
    if (next != NULL) *next = 0;
    return DISK_ERR_INVALID;
  }
  fat12_entry_loc_t loc = fat12_entry_locate(cluster);
  if (loc.sector >= FAT12_SECTORS_PER_FAT ||
      (loc.split && loc.sector + 1u >= FAT12_SECTORS_PER_FAT)) {
    *next = 0;
    return DISK_ERR_INVALID;
  }
  uint16_t lba = (uint16_t)(fat_start + loc.sector);
  uint8_t sector[DISK_SECTOR_SIZE];
  disk_err_t err = fat12_read_sector(fat, lba, sector);
  if (err != DISK_OK) return err;
  uint8_t hi;
  if (loc.split) {
    uint8_t next_sector[DISK_SECTOR_SIZE];
    err = fat12_read_sector(fat, lba + 1u, next_sector);
    if (err != DISK_OK) return err;
    hi = next_sector[0];
  } else {
    hi = sector[loc.offset + 1u];
  }
  *next = fat12_entry_unpack(cluster, sector[loc.offset], hi);
  return DISK_OK;
}

disk_err_t fat12_get_entry(fat12_t *fat, uint16_t cluster, uint16_t *next) {
  if (fat == NULL) return DISK_ERR_INVALID;
  return fat12_resolve_entry(fat, fat->fat_start, cluster, next);
}

static uint16_t fat12_root_lba(uint16_t index) {
  return (uint16_t)(FAT12_ROOT_START +
                    ((uint32_t)index * FAT12_DIR_ENTRY_SIZE) / DISK_SECTOR_SIZE);
}

static uint16_t fat12_root_offset(uint16_t index) {
  return (uint16_t)(((uint32_t)index * FAT12_DIR_ENTRY_SIZE) % DISK_SECTOR_SIZE);
}

disk_err_t fat12_read_root_entry(fat12_t *fat, uint16_t index,
                                 fat12_dirent_t *entry) {
  if (fat == NULL || entry == NULL) return DISK_ERR_INVALID;
  if (index >= FAT12_ROOT_ENTRIES) return DISK_END;
  uint8_t sector[DISK_SECTOR_SIZE];
  disk_err_t err = fat12_read_sector(fat, fat12_root_lba(index), sector);
  if (err != DISK_OK) return err;
  fat12_decode_dirent(entry, sector + fat12_root_offset(index));
  return DISK_OK;
}

bool fat12_entry_valid(const fat12_dirent_t *entry) {
  if (entry == NULL) return false;
  uint8_t first = (uint8_t)entry->name[0];
  return first != FAT12_DIRENT_END &&
         first != FAT12_DIRENT_FREE &&
         entry->attr != FAT12_ATTR_LFN;
}

bool fat12_entry_is_end(const fat12_dirent_t *entry) {
  return entry != NULL && (uint8_t)entry->name[0] == FAT12_DIRENT_END;
}

static char fat12_toupper(char value) {
  return value >= 'a' && value <= 'z' ? (char)(value - ('a' - 'A')) : value;
}

static bool fat12_name_char_valid(char value) {
  if ((value >= 'A' && value <= 'Z') ||
      (value >= 'a' && value <= 'z') ||
      (value >= '0' && value <= '9')) return true;
  switch (value) {
    case '$': case '%': case '\'': case '-': case '_': case '@': case '~':
    case '`': case '!': case '(': case ')': case '{': case '}': case '^':
    case '#': case '&':
      return true;
    default:
      return false;
  }
}

disk_err_t fat12_name_parse(const char *input, fat12_name_t *out) {
  if (input == NULL || out == NULL || *input == '\0' || *input == '.') {
    return DISK_ERR_INVALID;
  }
  memset(out->name, ' ', sizeof(out->name));
  memset(out->ext, ' ', sizeof(out->ext));

  size_t length = 0;
  while (*input != '\0' && *input != '.' && length < sizeof(out->name)) {
    if (!fat12_name_char_valid(*input)) return DISK_ERR_INVALID;
    out->name[length++] = fat12_toupper(*input++);
  }
  if (length == 0 || (*input != '\0' && *input != '.')) {
    return DISK_ERR_INVALID;
  }
  if (*input == '.') input++;

  length = 0;
  while (*input != '\0' && length < sizeof(out->ext)) {
    if (*input == '.' || !fat12_name_char_valid(*input)) {
      return DISK_ERR_INVALID;
    }
    out->ext[length++] = fat12_toupper(*input++);
  }
  return *input == '\0' ? DISK_OK : DISK_ERR_INVALID;
}

static bool fat12_name_matches(const fat12_dirent_t *entry,
                               const fat12_name_t *name) {
  return memcmp(entry->name, name->name, FAT12_FILENAME_LEN) == 0 &&
         memcmp(entry->ext, name->ext, FAT12_EXTENSION_LEN) == 0;
}

static disk_err_t fat12_root_lookup(fat12_t *fat, const fat12_name_t *name,
                                    fat12_lookup_t *out) {
  memset(out, 0, sizeof(*out));
  out->free_index = UINT16_MAX;
  for (uint16_t index = 0; index < FAT12_ROOT_ENTRIES; index++) {
    fat12_dirent_t entry;
    disk_err_t err = fat12_read_root_entry(fat, index, &entry);
    if (err != DISK_OK) return err;
    uint8_t first = (uint8_t)entry.name[0];
    if (first == FAT12_DIRENT_END) {
      if (out->free_index == UINT16_MAX) {
        out->free_index = index;
        out->free_is_end = true;
      }
      return DISK_OK;
    }
    if (first == FAT12_DIRENT_FREE) {
      if (out->free_index == UINT16_MAX) out->free_index = index;
      continue;
    }
    if (!fat12_entry_valid(&entry) ||
        (entry.attr & FAT12_ATTR_VOLUME_ID) != 0) continue;
    if (fat12_name_matches(&entry, name)) {
      out->found = true;
      out->index = index;
      out->entry = entry;
      return DISK_OK;
    }
  }
  return DISK_OK;
}

disk_err_t fat12_find(fat12_t *fat, const char *filename,
                      fat12_dirent_t *entry) {
  fat12_name_t name;
  if (fat == NULL || entry == NULL ||
      fat12_name_parse(filename, &name) != DISK_OK) {
    return DISK_ERR_INVALID;
  }
  fat12_lookup_t hit;
  disk_err_t err = fat12_root_lookup(fat, &name, &hit);
  if (err != DISK_OK) return err;
  if (!hit.found) return DISK_ERR_NOT_FOUND;
  *entry = hit.entry;
  return DISK_OK;
}

static disk_err_t fat12_read_cluster(fat12_t *fat, uint16_t cluster, uint8_t *buf) {
  if (!fat12_cluster_valid(cluster)) return DISK_ERR_INVALID;
  return fat12_read_sector(fat, fat12_cluster_to_lba(cluster), buf);
}

static disk_err_t fat12_validate_file_entry(fat12_t *fat,
                                            const fat12_dirent_t *entry) {
  if ((entry->attr & (FAT12_ATTR_DIRECTORY | FAT12_ATTR_VOLUME_ID)) != 0) {
    return DISK_ERR_INVALID;
  }
  uint32_t clusters = entry->size / DISK_SECTOR_SIZE +
      (entry->size % DISK_SECTOR_SIZE != 0);
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
  return fat12_is_eof(*next) || fat12_cluster_valid(*next)
      ? DISK_OK
      : DISK_ERR_CORRUPT;
}

static disk_err_t fat12_chain_at(fat12_t *fat, uint16_t start,
                                 uint32_t index, uint16_t *out) {
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
    disk_err_t err = fat12_read_cluster(
        file->fat, file->current_cluster, file->cluster_buf);
    if (err != DISK_OK) return err;
    file->buffer_valid = true;
  }
  return DISK_OK;
}

disk_result_t fat12_read(fat12_file_t *file, uint8_t *buf, size_t len) {
  disk_result_t result = { .error = DISK_OK, .count = 0 };
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

static disk_err_t fat12_set_entry(fat12_t *fat, uint16_t cluster,
                                  uint16_t value) {
  if (!fat12_cluster_valid(cluster) || value > 0x0FFFu) {
    return DISK_ERR_INVALID;
  }
  fat12_entry_loc_t loc = fat12_entry_locate(cluster);
  uint8_t sector[DISK_SECTOR_SIZE];
  uint8_t sector2[DISK_SECTOR_SIZE];
  disk_err_t err = fat12_read_sector(
      fat, (uint16_t)(fat->fat_start + loc.sector), sector);
  if (err != DISK_OK) return err;
  if (loc.split) {
    err = fat12_read_sector(
        fat, (uint16_t)(fat->fat_start + loc.sector + 1u), sector2);
    if (err != DISK_OK) return err;
  }
  uint8_t *hi = loc.split ? &sector2[0] : &sector[loc.offset + 1u];
  fat12_entry_pack(cluster, value, &sector[loc.offset], hi);

  for (uint8_t copy = 0; copy < FAT12_NUM_FATS; copy++) {
    uint16_t lba = (uint16_t)(FAT12_RESERVED_SECTORS +
        (uint16_t)copy * FAT12_SECTORS_PER_FAT + loc.sector);
    err = fat12_write_sector(fat, lba, sector);
    if (err != DISK_OK) return err;
    if (loc.split) {
      err = fat12_write_sector(fat, (uint16_t)(lba + 1u), sector2);
      if (err != DISK_OK) return err;
    }
  }
  return DISK_OK;
}

static void fat12_scan_init(fat12_scan_t *scan, fat12_t *fat,
                            uint16_t fat_start, uint16_t start) {
  memset(scan, 0, sizeof(*scan));
  scan->fat = fat;
  scan->cluster = start;
  scan->fat_start = fat_start;
  scan->cached_lba = UINT16_MAX;
  scan->cached_lba2 = UINT16_MAX;
}

static disk_err_t fat12_scan_next(fat12_scan_t *scan, uint16_t end,
                                  uint16_t *cluster, uint16_t *entry) {
  if (scan->cluster >= end) return DISK_END;
  fat12_entry_loc_t loc = fat12_entry_locate(scan->cluster);
  uint16_t lba = (uint16_t)(scan->fat_start + loc.sector);
  if (lba != scan->cached_lba) {
    disk_err_t err = fat12_read_sector(scan->fat, lba, scan->sector);
    if (err != DISK_OK) return err;
    scan->cached_lba = lba;
    scan->cached_lba2 = UINT16_MAX;
  }
  uint8_t hi;
  if (loc.split) {
    if (scan->cached_lba2 != lba + 1u) {
      disk_err_t err = fat12_read_sector(scan->fat, lba + 1u, scan->sector2);
      if (err != DISK_OK) return err;
      scan->cached_lba2 = lba + 1u;
    }
    hi = scan->sector2[0];
  } else {
    hi = scan->sector[loc.offset + 1u];
  }
  *cluster = scan->cluster;
  *entry = fat12_entry_unpack(scan->cluster, scan->sector[loc.offset], hi);
  scan->cluster++;
  return DISK_OK;
}

static disk_err_t fat12_find_free_range(fat12_t *fat, uint16_t start,
                                        uint16_t end, uint16_t *out) {
  fat12_scan_t scan;
  fat12_scan_init(&scan, fat, fat->fat_start, start);
  uint16_t cluster;
  uint16_t entry;
  disk_err_t err;
  while ((err = fat12_scan_next(&scan, end, &cluster, &entry)) == DISK_OK) {
    if (entry == 0) {
      *out = cluster;
      return DISK_OK;
    }
  }
  return err == DISK_END ? DISK_ERR_FULL : err;
}

static disk_err_t fat12_find_free_cluster(fat12_t *fat, uint16_t start,
                                          uint16_t *out) {
  if (start < 2 || start >= FAT12_CLUSTER_LIMIT) start = 2;
  disk_err_t err = fat12_find_free_range(fat, start, FAT12_CLUSTER_LIMIT, out);
  if (err != DISK_ERR_FULL || start == 2) return err;
  return fat12_find_free_range(fat, 2, start, out);
}

disk_err_t fat12_free_count(fat12_t *fat, uint16_t *count) {
  if (fat == NULL || count == NULL) return DISK_ERR_INVALID;
  fat12_scan_t scan;
  fat12_scan_init(&scan, fat, fat->fat_start, 2);
  uint16_t cluster;
  uint16_t entry;
  uint16_t total = 0;
  disk_err_t err;
  while ((err = fat12_scan_next(
              &scan, FAT12_CLUSTER_LIMIT, &cluster, &entry)) == DISK_OK) {
    if (entry == 0) total++;
  }
  if (err != DISK_END) return err;
  *count = total;
  return DISK_OK;
}

static disk_err_t fat12_write_cluster(fat12_t *fat, uint16_t cluster,
                                      const uint8_t *buf) {
  if (!fat12_cluster_valid(cluster)) return DISK_ERR_INVALID;
  return fat12_write_sector(fat, fat12_cluster_to_lba(cluster), buf);
}

static disk_err_t fat12_write_root_entry(fat12_t *fat, uint16_t index,
                                         const fat12_dirent_t *entry) {
  if (index >= FAT12_ROOT_ENTRIES) return DISK_END;
  uint16_t lba = fat12_root_lba(index);
  uint8_t sector[DISK_SECTOR_SIZE];
  disk_err_t err = fat12_read_sector(fat, lba, sector);
  if (err != DISK_OK) return err;
  fat12_encode_dirent(sector + fat12_root_offset(index), entry);
  return fat12_write_sector(fat, lba, sector);
}

static void fat12_init_dirent(fat12_dirent_t *entry,
                              const fat12_name_t *name) {
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
      report.duplicate_names == 0 &&
      !report.fat_mismatch && !report.fat_markers_invalid
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
    writer->replacing_existing = true;
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
      offset == 0 ? DISK_SECTOR_SIZE : offset;
}

static disk_err_t fat12_writer_flush_cluster(fat12_writer_t *writer) {
  if (writer->current_cluster == 0) return DISK_OK;
  return fat12_write_cluster(
      writer->fat, writer->current_cluster, writer->cluster_buf);
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
  disk_result_t result = { .error = DISK_OK, .count = 0 };
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
  fat12_end(writer->fat);
  writer->phase = FAT12_WRITER_DONE;
  return DISK_OK;
}

void fat12_forget_write(fat12_writer_t *writer) {
  if (writer == NULL) return;
  if (writer->fat != NULL) fat12_end(writer->fat);
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
        err = fat12_flush(fat);
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
        err = fat12_flush(fat);
        if (err != DISK_OK) return err;
        writer->phase = FAT12_WRITER_PREPARE_RECLAIM;
        break;

      case FAT12_WRITER_PREPARE_RECLAIM:
        if (!writer->replacing_existing || writer->dirent.start_cluster < 2) {
          writer->phase = FAT12_WRITER_DONE;
          fat12_end(fat);
          return DISK_OK;
        }
        err = fat12_stage_free_chain(fat, writer->dirent.start_cluster);
        if (err != DISK_OK) return err;
        writer->phase = FAT12_WRITER_FLUSH_RECLAIM;
        break;

      case FAT12_WRITER_FLUSH_RECLAIM:
        err = fat12_flush(fat);
        if (err != DISK_OK) return err;
        writer->phase = FAT12_WRITER_DONE;
        fat12_end(fat);
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
  if (err == DISK_OK) err = fat12_flush(fat);
  if (err == DISK_OK && start >= 2) {
    err = fat12_stage_free_chain(fat, start);
    if (err == DISK_OK) err = fat12_flush(fat);
  }
  fat12_end(fat);
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
  if (err == DISK_OK) err = fat12_flush(fat);
  fat12_end(fat);
  return err;
}

static disk_err_t fsck_get_entry(fsck_ctx_t *ctx, uint16_t cluster,
                                 uint16_t *next) {
  return fat12_resolve_entry(ctx->fat, ctx->fat_start, cluster, next);
}

static void fsck_bit_set(uint8_t *map, uint16_t cluster) {
  map[cluster / 8u] |= (uint8_t)(1u << (cluster % 8u));
}

static bool fsck_bit_test(const uint8_t *map, uint16_t cluster) {
  return (map[cluster / 8u] & (1u << (cluster % 8u))) != 0;
}

static uint16_t fsck_pending_pop(fat12_t *fat) {
  for (uint16_t byte = 0; byte < FAT12_CLUSTER_BITMAP_BYTES; byte++) {
    if (fat->fsck_pending[byte] == 0) continue;
    uint16_t bit = 0;
    while ((fat->fsck_pending[byte] & (1u << bit)) == 0) bit++;
    fat->fsck_pending[byte] &= (uint8_t)~(1u << bit);
    return (uint16_t)(byte * 8u + bit);
  }
  return 0;
}

static disk_err_t fsck_plan_fat(fsck_ctx_t *ctx, uint16_t cluster,
                                uint16_t value) {
  if (ctx->mode != FSCK_REPAIR_FAT || !fat12_cluster_valid(cluster)) {
    return DISK_OK;
  }
  disk_err_t err = fat12_set_entry(ctx->fat, cluster, value);
  if (err == DISK_OK && value == 0) ctx->freed++;
  return err;
}

static disk_err_t fsck_cut(fsck_ctx_t *ctx, uint16_t cluster) {
  disk_err_t err = fsck_plan_fat(ctx, cluster, FAT12_EOC);
  if (err == DISK_OK && ctx->mode == FSCK_REPAIR_FAT) ctx->tail_cuts++;
  return err;
}

static disk_err_t fsck_rewrite_entry(fsck_ctx_t *ctx, fsck_slot_t slot,
                                     uint16_t start_cluster, uint32_t size) {
  if (ctx->mode != FSCK_REPAIR_DIR) return DISK_OK;
  uint8_t sector[DISK_SECTOR_SIZE];
  disk_err_t err = fat12_read_sector(ctx->fat, slot.lba, sector);
  if (err != DISK_OK) return err;
  fat12_dirent_t entry;
  fat12_decode_dirent(&entry, sector + slot.offset);
  entry.start_cluster = start_cluster;
  entry.size = size;
  fat12_encode_dirent(sector + slot.offset, &entry);
  err = fat12_write_sector(ctx->fat, slot.lba, sector);
  if (err == DISK_OK) ctx->truncated++;
  return err;
}

static disk_err_t fsck_remove_entry(fsck_ctx_t *ctx, fsck_slot_t slot,
                                    bool directory, bool duplicate) {
  if (ctx->mode != FSCK_REPAIR_DIR) return DISK_OK;
  uint8_t sector[DISK_SECTOR_SIZE];
  disk_err_t err = fat12_read_sector(ctx->fat, slot.lba, sector);
  if (err != DISK_OK) return err;
  sector[slot.offset] = FAT12_DIRENT_FREE;
  err = fat12_write_sector(ctx->fat, slot.lba, sector);
  if (err != DISK_OK) return err;
  if (directory) ctx->removed++;
  if (duplicate) ctx->duplicates_removed++;
  return DISK_OK;
}

static void fsck_count_collision(fsck_ctx_t *ctx, bool loop) {
  if (loop) {
    ctx->out->loops++;
    ctx->out->broken_chains++;
  } else {
    ctx->out->crosslinked++;
  }
}

static disk_err_t fsck_follow(fsck_ctx_t *ctx, uint16_t cluster,
                              uint16_t owner, uint16_t *next,
                              fsck_link_t *link) {
  ctx->fat->fsck_owner[cluster] = owner;
  disk_err_t err = fsck_get_entry(ctx, cluster, next);
  if (err != DISK_OK) return err;
  uint16_t value = *next;
  if (fat12_is_bad(value)) *link = LINK_BAD;
  else if (fat12_is_eof(value)) *link = LINK_END;
  else if (!fat12_cluster_valid(value)) *link = LINK_BROKEN;
  else if (ctx->fat->fsck_owner[value] == owner) *link = LINK_LOOP;
  else if (ctx->fat->fsck_owner[value] != 0) *link = LINK_CROSS;
  else *link = LINK_NEXT;
  return DISK_OK;
}

static disk_err_t fsck_walk_file(fsck_ctx_t *ctx, const fat12_dirent_t *entry,
                                 fsck_slot_t slot) {
  uint32_t declared = entry->size / DISK_SECTOR_SIZE +
      (entry->size % DISK_SECTOR_SIZE != 0);
  bool oversized = declared > FAT12_DATA_CLUSTERS;
  uint16_t expected = oversized ? FAT12_DATA_CLUSTERS : (uint16_t)declared;
  uint16_t start = entry->start_cluster;
  if (entry->size == 0) {
    if (start == 0) return DISK_OK;
    ctx->out->size_mismatches++;
    return fsck_rewrite_entry(ctx, slot, 0, 0);
  }
  if (!fat12_cluster_valid(start)) {
    ctx->out->broken_chains++;
    ctx->out->size_mismatches++;
    return fsck_rewrite_entry(ctx, slot, 0, 0);
  }
  if (ctx->fat->fsck_owner[start] != 0) {
    ctx->out->crosslinked++;
    ctx->out->size_mismatches++;
    return fsck_rewrite_entry(ctx, slot, 0, 0);
  }

  uint16_t owner = ctx->next_owner++;
  uint16_t cluster = start;
  uint16_t previous = 0;
  uint16_t count = 0;
  for (;;) {
    count++;
    uint16_t next;
    fsck_link_t link;
    disk_err_t err = fsck_follow(ctx, cluster, owner, &next, &link);
    if (err != DISK_OK) return err;
    if (link == LINK_BAD) {
      ctx->fat->fsck_owner[cluster] = 0;
      ctx->out->broken_chains++;
      ctx->out->size_mismatches++;
      err = fsck_plan_fat(ctx, previous, FAT12_EOC);
      if (err != DISK_OK) return err;
      return fsck_rewrite_entry(ctx, slot, previous == 0 ? 0 : start,
                                (uint32_t)(count - 1u) * DISK_SECTOR_SIZE);
    }
    if (count == expected) {
      if (oversized) {
        ctx->out->broken_chains++;
        ctx->out->size_mismatches++;
        if (link != LINK_END) {
          err = fsck_cut(ctx, cluster);
          if (err != DISK_OK) return err;
        }
        return fsck_rewrite_entry(ctx, slot, start,
                                  (uint32_t)count * DISK_SECTOR_SIZE);
      }
      if (link != LINK_END) {
        ctx->out->size_mismatches++;
        if (link == LINK_LOOP || link == LINK_CROSS) {
          fsck_count_collision(ctx, link == LINK_LOOP);
        }
        return fsck_cut(ctx, cluster);
      }
      return DISK_OK;
    }
    if (link == LINK_NEXT) {
      previous = cluster;
      cluster = next;
      continue;
    }
    ctx->out->size_mismatches++;
    if (link == LINK_END || link == LINK_BROKEN) ctx->out->broken_chains++;
    else fsck_count_collision(ctx, link == LINK_LOOP);
    if (link != LINK_END) {
      err = fsck_plan_fat(ctx, cluster, FAT12_EOC);
      if (err != DISK_OK) return err;
    }
    return fsck_rewrite_entry(ctx, slot, start,
                              (uint32_t)count * DISK_SECTOR_SIZE);
  }
}

static bool fsck_namespace_entry(const fat12_dirent_t *entry) {
  return fat12_entry_valid(entry) &&
      (entry->attr & FAT12_ATTR_VOLUME_ID) == 0 && entry->name[0] != '.';
}

static bool fsck_same_name(const fat12_dirent_t *left,
                           const fat12_dirent_t *right) {
  return memcmp(left->name, right->name, FAT12_FILENAME_LEN) == 0 &&
      memcmp(left->ext, right->ext, FAT12_EXTENSION_LEN) == 0;
}

static disk_err_t fsck_duplicate(fsck_ctx_t *ctx, uint16_t directory,
                                 fsck_slot_t slot, const fat12_dirent_t *entry,
                                 bool *duplicate) {
  *duplicate = false;
  uint16_t cluster = directory;
  for (uint16_t index = 0;; index++) {
    uint16_t lba = directory == 0 ? (uint16_t)(FAT12_ROOT_START + index)
                                  : fat12_cluster_to_lba(cluster);
    uint8_t sector[DISK_SECTOR_SIZE];
    disk_err_t err = fat12_read_sector(ctx->fat, lba, sector);
    if (err != DISK_OK) return err;
    for (uint16_t offset = 0; offset < DISK_SECTOR_SIZE;
         offset += FAT12_DIR_ENTRY_SIZE) {
      if (lba == slot.lba && offset == slot.offset) return DISK_OK;
      fat12_dirent_t candidate;
      fat12_decode_dirent(&candidate, sector + offset);
      if (fsck_namespace_entry(&candidate) && fsck_same_name(&candidate, entry)) {
        *duplicate = true;
        return DISK_OK;
      }
    }
    if (directory != 0) {
      uint16_t next;
      err = fsck_get_entry(ctx, cluster, &next);
      if (err != DISK_OK) return err;
      cluster = next;
    }
  }
}

static disk_err_t fsck_admit_directory(fsck_ctx_t *ctx, uint16_t start,
                                       fsck_slot_t slot) {
  if (!fat12_cluster_valid(start)) {
    ctx->out->broken_chains++;
    return fsck_remove_entry(ctx, slot, true, false);
  }
  if (ctx->fat->fsck_owner[start] != 0) {
    ctx->out->crosslinked++;
    return fsck_remove_entry(ctx, slot, true, false);
  }
  uint16_t link;
  disk_err_t err = fsck_get_entry(ctx, start, &link);
  if (err != DISK_OK) return err;
  if (fat12_is_bad(link)) {
    ctx->out->broken_chains++;
    return fsck_remove_entry(ctx, slot, true, false);
  }
  ctx->fat->fsck_owner[start] = ctx->next_owner++;
  fsck_bit_set(ctx->fat->fsck_pending, start);
  return DISK_OK;
}

static disk_err_t fsck_scan_dirent(fsck_ctx_t *ctx, const uint8_t *raw,
                                   fsck_slot_t slot, uint16_t directory,
                                   bool *end) {
  fat12_dirent_t entry;
  fat12_decode_dirent(&entry, raw);
  if (fat12_entry_is_end(&entry)) {
    *end = true;
    return DISK_OK;
  }
  if (!fsck_namespace_entry(&entry)) return DISK_OK;
  bool duplicate;
  disk_err_t err = fsck_duplicate(ctx, directory, slot, &entry, &duplicate);
  if (err != DISK_OK) return err;
  bool subdirectory = (entry.attr & FAT12_ATTR_DIRECTORY) != 0;
  if (duplicate) {
    ctx->out->duplicate_names++;
    if (ctx->mode == FSCK_REPAIR_DIR) {
      return fsck_remove_entry(ctx, slot, subdirectory, true);
    }
  }
  if (subdirectory) {
    ctx->out->directories++;
    return fsck_admit_directory(ctx, entry.start_cluster, slot);
  }
  ctx->out->files++;
  return fsck_walk_file(ctx, &entry, slot);
}

static disk_err_t fsck_scan_sector(fsck_ctx_t *ctx, uint16_t lba,
                                   uint16_t directory, bool *end) {
  uint8_t sector[DISK_SECTOR_SIZE];
  disk_err_t err = fat12_read_sector(ctx->fat, lba, sector);
  if (err != DISK_OK) return err;
  for (uint16_t offset = 0; offset < DISK_SECTOR_SIZE && !*end;
       offset += FAT12_DIR_ENTRY_SIZE) {
    fsck_slot_t slot = {.lba = lba, .offset = offset};
    err = fsck_scan_dirent(ctx, sector + offset, slot, directory, end);
    if (err != DISK_OK) return err;
  }
  return DISK_OK;
}

static disk_err_t fsck_walk_directory(fsck_ctx_t *ctx, uint16_t start) {
  uint16_t owner = ctx->fat->fsck_owner[start];
  uint16_t cluster = start;
  uint16_t previous = 0;
  bool end = false;
  for (;;) {
    uint16_t next;
    fsck_link_t link;
    disk_err_t err = fsck_follow(ctx, cluster, owner, &next, &link);
    if (err != DISK_OK) return err;
    if (link == LINK_BAD) {
      ctx->fat->fsck_owner[cluster] = 0;
      ctx->out->broken_chains++;
      return fsck_plan_fat(ctx, previous, FAT12_EOC);
    }
    if (!end) {
      err = fsck_scan_sector(ctx, fat12_cluster_to_lba(cluster), start, &end);
      if (err != DISK_OK) return err;
    }
    if (link == LINK_END) return DISK_OK;
    if (link == LINK_NEXT) {
      previous = cluster;
      cluster = next;
      continue;
    }
    if (link == LINK_BROKEN) ctx->out->broken_chains++;
    else fsck_count_collision(ctx, link == LINK_LOOP);
    return fsck_plan_fat(ctx, cluster, FAT12_EOC);
  }
}

static disk_err_t fsck_scan_tree(fsck_ctx_t *ctx) {
  bool end = false;
  for (uint16_t sector = 0; sector < FAT12_ROOT_DIR_SECTORS && !end; sector++) {
    disk_err_t err = fsck_scan_sector(ctx, FAT12_ROOT_START + sector, 0, &end);
    if (err != DISK_OK) return err;
  }
  for (uint16_t start; (start = fsck_pending_pop(ctx->fat)) != 0;) {
    disk_err_t err = fsck_walk_directory(ctx, start);
    if (err != DISK_OK) return err;
  }
  return DISK_OK;
}

static disk_err_t fsck_scan_lost(fsck_ctx_t *ctx) {
  fat12_scan_t scan;
  fat12_scan_init(&scan, ctx->fat, ctx->fat_start, 2);
  uint16_t cluster;
  uint16_t entry;
  disk_err_t err;
  while ((err = fat12_scan_next(
              &scan, FAT12_CLUSTER_LIMIT, &cluster, &entry)) == DISK_OK) {
    if (entry == 0 || fat12_is_bad(entry)) continue;
    if (ctx->fat->fsck_owner[cluster] != 0) continue;
    ctx->out->lost_clusters++;
    err = fsck_plan_fat(ctx, cluster, 0);
    if (err != DISK_OK) return err;
  }
  return err == DISK_END ? DISK_OK : err;
}

static disk_err_t fsck_scan(fsck_ctx_t *ctx) {
  disk_err_t err = fsck_scan_tree(ctx);
  if (err != DISK_OK) return err;
  return fsck_scan_lost(ctx);
}

static disk_err_t fsck_compare_fats(fat12_t *fat, fat12_mirrors_t *mirrors) {
  uint8_t primary[DISK_SECTOR_SIZE];
  uint8_t copy[DISK_SECTOR_SIZE];
  memset(mirrors, 0, sizeof(*mirrors));
  for (uint16_t sector = 0; sector < FAT12_SECTORS_PER_FAT; sector++) {
    disk_err_t err = fat12_read_sector(
        fat, (uint16_t)(FAT12_FAT1_START + sector), primary);
    if (err != DISK_OK) return err;
    if (sector == 0) {
      mirrors->valid[0] = primary[0] == FAT12_MEDIA_DESCRIPTOR &&
          primary[1] == 0xFF && primary[2] == 0xFF;
    }
    for (uint8_t index = 1; index < FAT12_NUM_FATS; index++) {
      uint16_t lba = (uint16_t)(FAT12_FAT1_START +
          (uint16_t)index * FAT12_SECTORS_PER_FAT + sector);
      err = fat12_read_sector(fat, lba, copy);
      if (err != DISK_OK) return err;
      if (sector == 0) {
        mirrors->valid[index] = copy[0] == FAT12_MEDIA_DESCRIPTOR &&
            copy[1] == 0xFF && copy[2] == 0xFF;
      }
      if (memcmp(primary, copy, DISK_SECTOR_SIZE) != 0) {
        mirrors->mismatch = true;
      }
    }
  }
  return DISK_OK;
}

static void fsck_reset(fsck_ctx_t *ctx, fat12_t *fat, fat12_fsck_t *out,
                       fsck_mode_t mode, uint16_t fat_start) {
  memset(fat->fsck_owner, 0, sizeof(fat->fsck_owner));
  memset(fat->fsck_pending, 0, sizeof(fat->fsck_pending));
  memset(ctx, 0, sizeof(*ctx));
  ctx->fat = fat;
  ctx->out = out;
  ctx->mode = mode;
  ctx->next_owner = 1;
  ctx->fat_start = fat_start;
}

static void fsck_capture_reachable(fat12_t *fat) {
  memset(fat->fsck_reachable, 0, sizeof(fat->fsck_reachable));
  for (uint16_t cluster = 2; cluster < FAT12_CLUSTER_LIMIT; cluster++) {
    if (fat->fsck_owner[cluster] != 0) fsck_bit_set(fat->fsck_reachable, cluster);
  }
}

static bool fsck_was_reachable(const fat12_t *fat, uint16_t cluster) {
  return fsck_bit_test(fat->fsck_reachable, cluster);
}

static uint32_t fsck_structural_score(const fat12_fsck_t *report) {
  uint32_t score = 0;
  score += report->crosslinked;
  score += report->loops;
  score += report->broken_chains;
  score += report->size_mismatches;
  return score;
}

static uint32_t fsck_candidate_score(const fat12_fsck_t *report,
                                     bool marker_valid) {
  return fsck_structural_score(report) + (marker_valid ? 0u : 1u);
}

static disk_err_t fsck_reachable_equal(fat12_t *fat, bool *equal) {
  *equal = true;
  for (uint16_t cluster = 2; cluster < FAT12_CLUSTER_LIMIT; cluster++) {
    bool primary = fsck_was_reachable(fat, cluster);
    bool secondary = fat->fsck_owner[cluster] != 0;
    if (primary != secondary) {
      *equal = false;
      return DISK_OK;
    }
    if (!primary) continue;
    uint16_t first;
    uint16_t second;
    disk_err_t err = fat12_resolve_entry(fat, FAT12_FAT1_START, cluster, &first);
    if (err != DISK_OK) return err;
    err = fat12_resolve_entry(fat, FAT12_FAT2_START, cluster, &second);
    if (err != DISK_OK) return err;
    if (first != second && !(fat12_is_eof(first) && fat12_is_eof(second))) {
      *equal = false;
      return DISK_OK;
    }
  }
  return DISK_OK;
}

static disk_err_t fsck_select_fat(fat12_t *fat,
                                  const fat12_mirrors_t *mirrors,
                                  fat12_fsck_t *out,
                                  uint16_t *fat_start) {
  fsck_ctx_t ctx;
  fat12_fsck_t primary;
  memset(&primary, 0, sizeof(primary));
  fsck_reset(&ctx, fat, &primary, FSCK_REPORT, FAT12_FAT1_START);
  disk_err_t err = fsck_scan(&ctx);
  if (err != DISK_OK) return err;
  fsck_capture_reachable(fat);
  uint32_t primary_score = fsck_candidate_score(&primary, mirrors->valid[0]);

  if (!mirrors->mismatch) {
    *out = primary;
    out->fat1_score = primary_score;
    out->fat2_score = primary_score;
    out->authoritative_fat = 1;
    *fat_start = FAT12_FAT1_START;
    return DISK_OK;
  }

  fat12_fsck_t secondary;
  memset(&secondary, 0, sizeof(secondary));
  fsck_reset(&ctx, fat, &secondary, FSCK_REPORT, FAT12_FAT2_START);
  err = fsck_scan(&ctx);
  if (err != DISK_OK) return err;
  uint32_t secondary_score = fsck_candidate_score(&secondary, mirrors->valid[1]);
  bool primary_clean = primary_score == 0;
  bool secondary_clean = secondary_score == 0;
  bool choose_primary = false;
  bool choose_secondary = false;
  if (primary_clean && !secondary_clean) {
    choose_primary = true;
  } else if (!primary_clean && secondary_clean) {
    choose_secondary = true;
  } else if (primary_clean && secondary_clean) {
    bool equal;
    err = fsck_reachable_equal(fat, &equal);
    if (err != DISK_OK) return err;
    choose_primary = equal;
  } else if (!mirrors->valid[0] && !mirrors->valid[1]) {
    uint32_t primary_structure = fsck_structural_score(&primary);
    uint32_t secondary_structure = fsck_structural_score(&secondary);
    if (primary_structure == 0 && secondary_structure != 0) {
      choose_primary = true;
    } else if (primary_structure != 0 && secondary_structure == 0) {
      choose_secondary = true;
    } else if (primary_structure == 0 && secondary_structure == 0) {
      bool equal;
      err = fsck_reachable_equal(fat, &equal);
      if (err != DISK_OK) return err;
      choose_primary = equal;
    }
  }

  if (!choose_primary && !choose_secondary) {
    memset(out, 0, sizeof(*out));
    out->fat_mismatch = true;
    out->fat_markers_invalid = !mirrors->valid[0] || !mirrors->valid[1];
    out->fat_ambiguous = true;
    out->fat1_score = primary_score;
    out->fat2_score = secondary_score;
    return DISK_ERR_AMBIGUOUS;
  }

  if (choose_primary) {
    *out = primary;
    *fat_start = FAT12_FAT1_START;
  } else {
    *out = secondary;
    *fat_start = FAT12_FAT2_START;
    fsck_capture_reachable(fat);
  }
  out->fat_mismatch = true;
  out->fat1_score = primary_score;
  out->fat2_score = secondary_score;
  out->authoritative_fat = choose_primary ? 1u : 2u;
  return DISK_OK;
}

static disk_err_t fsck_stage_fat_copy(fat12_t *fat, uint16_t source,
                                      uint16_t target) {
  for (uint16_t index = 0; index < FAT12_SECTORS_PER_FAT; index++) {
    uint8_t sector[DISK_SECTOR_SIZE];
    disk_err_t err = fat12_read_sector(fat, source + index, sector);
    if (err != DISK_OK) return err;
    err = fat12_write_sector(fat, target + index, sector);
    if (err != DISK_OK) return err;
  }
  return DISK_OK;
}

static disk_err_t fsck_stage_markers(fat12_t *fat) {
  for (uint8_t copy = 0; copy < FAT12_NUM_FATS; copy++) {
    uint16_t lba = (uint16_t)(FAT12_FAT1_START +
        (uint16_t)copy * FAT12_SECTORS_PER_FAT);
    uint8_t sector[DISK_SECTOR_SIZE];
    disk_err_t err = fat12_read_sector(fat, lba, sector);
    if (err != DISK_OK) return err;
    sector[0] = FAT12_MEDIA_DESCRIPTOR;
    sector[1] = 0xFF;
    sector[2] = 0xFF;
    err = fat12_write_sector(fat, lba, sector);
    if (err != DISK_OK) return err;
  }
  return DISK_OK;
}

static disk_err_t fsck_preserve_bad_union(fat12_t *fat) {
  for (uint16_t cluster = 2; cluster < FAT12_CLUSTER_LIMIT; cluster++) {
    if (fsck_was_reachable(fat, cluster)) continue;
    uint16_t first;
    uint16_t second;
    disk_err_t err = fat12_resolve_entry(fat, FAT12_FAT1_START, cluster, &first);
    if (err != DISK_OK) return err;
    err = fat12_resolve_entry(fat, FAT12_FAT2_START, cluster, &second);
    if (err != DISK_OK) return err;
    if (fat12_is_bad(first) || fat12_is_bad(second)) {
      err = fat12_set_entry(fat, cluster, FAT12_BAD);
      if (err != DISK_OK) return err;
    }
  }
  return DISK_OK;
}

disk_err_t fat12_fsck(fat12_t *fat, fat12_fsck_t *out, bool repair) {
  if (fat == NULL || out == NULL) return DISK_ERR_INVALID;
  if (fat12_busy(fat)) return DISK_ERR_BUSY;
  if (repair) {
    disk_err_t err = cache_writable(fat->cache);
    if (err != DISK_OK) return err;
  }
  memset(out, 0, sizeof(*out));
  fat12_mirrors_t mirrors;
  disk_err_t err = fsck_compare_fats(fat, &mirrors);
  if (err != DISK_OK) return err;
  uint16_t selected_fat;
  err = fsck_select_fat(fat, &mirrors, out, &selected_fat);
  if (err != DISK_OK) return err;
  fat->fat_start = selected_fat;
  out->fat_mismatch = mirrors.mismatch;
  out->fat_markers_invalid = false;
  for (uint8_t index = 0; index < FAT12_NUM_FATS; index++) {
    out->fat_markers_invalid |= !mirrors.valid[index];
  }
  if (!repair) return DISK_OK;

  fat->transaction = true;
  fsck_ctx_t ctx;
  fat12_fsck_t pass;
  memset(&pass, 0, sizeof(pass));
  fsck_reset(&ctx, fat, &pass, FSCK_REPAIR_DIR, selected_fat);
  err = fsck_scan_tree(&ctx);
  out->truncated_files = ctx.truncated;
  out->removed_directories = ctx.removed;
  out->removed_duplicates = ctx.duplicates_removed;
  if (err == DISK_OK) err = fat12_flush(fat);
  if (err != DISK_OK) {
    fat12_end(fat);
    return err;
  }

  memset(&pass, 0, sizeof(pass));
  fsck_reset(&ctx, fat, &pass, FSCK_REPAIR_FAT, selected_fat);
  err = fsck_scan_tree(&ctx);
  if (err == DISK_OK) {
    fsck_capture_reachable(fat);
    if (mirrors.mismatch) err = fsck_preserve_bad_union(fat);
  }
  if (err == DISK_OK) err = fsck_scan_lost(&ctx);
  bool repaired_fat1 = !mirrors.valid[0];
  bool repaired_fat2 = !mirrors.valid[1];
  if (err == DISK_OK && mirrors.mismatch) {
    uint16_t target = selected_fat == FAT12_FAT1_START
        ? FAT12_FAT2_START : FAT12_FAT1_START;
    err = fsck_stage_fat_copy(fat, selected_fat, target);
    repaired_fat1 |= target == FAT12_FAT1_START;
    repaired_fat2 |= target == FAT12_FAT2_START;
  }
  if (err == DISK_OK && out->fat_markers_invalid) err = fsck_stage_markers(fat);
  if (err == DISK_OK) {
    out->freed = ctx.freed;
    out->freed_tails = ctx.tail_cuts;
    err = fat12_flush(fat);
  }
  if (err == DISK_OK) {
    out->repaired_fat1 = repaired_fat1;
    out->repaired_fat2 = repaired_fat2;
    fat->fat_start = FAT12_FAT1_START;
  }
  fat12_end(fat);
  return err;
}

static disk_err_t fat12_volume_label_validate(const char *label,
                                              size_t *length) {
  *length = 0;
  if (label == NULL) return DISK_OK;
  while (*length <= 11 && label[*length] != '\0') {
    unsigned char value = (unsigned char)label[*length];
    if (value < 0x20 || value > 0x7E ||
        strchr("\"*+,./:;<=>?[\\]|", value) != NULL) {
      return DISK_ERR_INVALID;
    }
    (*length)++;
  }
  return *length > 11 || *length == 0 ? DISK_ERR_INVALID : DISK_OK;
}

static void fat12_format_sector(uint8_t *sector, uint16_t lba,
                                const char *label, size_t label_length) {
  memset(sector, 0, DISK_SECTOR_SIZE);
  if (lba == 0) {
    sector[0] = 0xEB;
    sector[1] = 0x3C;
    sector[2] = 0x90;
    memcpy(sector + 3, "MSDOS5.0", 8);
    fat12_encode_bpb(sector);
    sector[38] = 0x29;
    sector[39] = 0x12;
    sector[40] = 0x34;
    sector[41] = 0x56;
    sector[42] = 0x78;
    memset(sector + 43, ' ', 11);
    if (label != NULL) {
      for (size_t index = 0; index < label_length; index++) {
        sector[43 + index] = (uint8_t)fat12_toupper(label[index]);
      }
    } else {
      memcpy(sector + 43, "NO NAME", 7);
    }
    memcpy(sector + 54, "FAT12   ", 8);
    sector[FAT12_BOOT_SIG_OFFSET] = 0x55;
    sector[FAT12_BOOT_SIG_OFFSET + 1] = 0xAA;
    return;
  }
  if (lba == FAT12_FAT1_START || lba == FAT12_FAT2_START) {
    sector[0] = FAT12_MEDIA_DESCRIPTOR;
    sector[1] = 0xFF;
    sector[2] = 0xFF;
    return;
  }
  if (lba == FAT12_ROOT_START && label != NULL) {
    fat12_dirent_t entry;
    memset(&entry, 0, sizeof(entry));
    memset(entry.name, ' ', sizeof(entry.name));
    memset(entry.ext, ' ', sizeof(entry.ext));
    for (size_t index = 0; index < label_length; index++) {
      char value = fat12_toupper(label[index]);
      if (index < sizeof(entry.name)) entry.name[index] = value;
      else entry.ext[index - sizeof(entry.name)] = value;
    }
    entry.attr = FAT12_ATTR_VOLUME_ID;
    fat12_encode_dirent(sector, &entry);
  }
}

disk_err_t fat12_format(fat12_t *fat, cache_t *cache, const char *volume_label,
                        bool write_all_tracks, fat12_progress_t progress,
                        void *progress_ctx) {
  if (fat == NULL || cache == NULL) return DISK_ERR_INVALID;
  size_t label_length;
  disk_err_t err = fat12_volume_label_validate(volume_label, &label_length);
  if (err != DISK_OK) return err;
  err = cache_writable(cache);
  if (err != DISK_OK) return err;
  memset(fat, 0, sizeof(*fat));
  fat->cache = cache;
  fat->fat_start = FAT12_FAT1_START;

  uint16_t tracks = (uint16_t)(write_all_tracks
      ? DISK_TRACK_COUNT
      : (FAT12_DATA_START + DISK_SECTORS_PER_TRACK - 1u) /
        DISK_SECTORS_PER_TRACK);
  uint8_t sector[DISK_SECTOR_SIZE];
  for (uint16_t track = 0; track < tracks; track++) {
    for (uint8_t index = 0; index < DISK_SECTORS_PER_TRACK; index++) {
      uint16_t lba = (uint16_t)(track * DISK_SECTORS_PER_TRACK + index);
      fat12_format_sector(sector, lba, volume_label, label_length);
      err = cache_write(cache, lba, sector);
      if (err != DISK_OK) return err;
    }
    err = cache_flush(cache);
    if (err != DISK_OK) return err;
    if (progress != NULL) {
      progress(progress_ctx, (uint8_t)(track / DISK_HEADS),
               (uint8_t)(track % DISK_HEADS), (uint16_t)(track + 1u), tracks);
    }
  }
  return DISK_OK;
}

bool fat12_fsck_clean(const fat12_fsck_t *report) {
  return report != NULL && report->lost_clusters == 0 &&
         report->crosslinked == 0 && report->loops == 0 &&
         report->broken_chains == 0 && report->size_mismatches == 0 &&
         report->truncated_files == 0 && report->duplicate_names == 0 &&
         !report->fat_mismatch && !report->fat_markers_invalid &&
         !report->fat_ambiguous;
}
