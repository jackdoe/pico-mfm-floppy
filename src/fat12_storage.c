#include "internal/fat12_internal.h"
#include "internal/byteorder.h"
#include <string.h>

void fat12_decode_dirent(fat12_dirent_t *entry, const uint8_t *raw) {
  memcpy(entry->name, raw, FAT12_FILENAME_LEN);
  memcpy(entry->ext, raw + 8, FAT12_EXTENSION_LEN);
  entry->attr = raw[11];
  memcpy(entry->reserved, raw + 12, sizeof(entry->reserved));
  entry->time = load_le16(raw + 22);
  entry->date = load_le16(raw + 24);
  entry->start_cluster = load_le16(raw + 26);
  entry->size = load_le32(raw + 28);
}

void fat12_encode_dirent(uint8_t *raw, const fat12_dirent_t *entry) {
  memcpy(raw, entry->name, FAT12_FILENAME_LEN);
  memcpy(raw + 8, entry->ext, FAT12_EXTENSION_LEN);
  raw[11] = entry->attr;
  memcpy(raw + 12, entry->reserved, sizeof(entry->reserved));
  store_le16(raw + 22, entry->time);
  store_le16(raw + 24, entry->date);
  store_le16(raw + 26, entry->start_cluster);
  store_le32(raw + 28, entry->size);
}

bool fat12_is_eof(uint16_t cluster) {
  return cluster >= 0x0FF8u && cluster <= 0x0FFFu;
}

disk_err_t fat12_resolve_entry(fat12_t *fat, uint16_t fat_start,
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
  disk_err_t err = cache_read(fat->cache, lba, sector);
  if (err != DISK_OK) return err;
  uint8_t hi;
  if (loc.split) {
    uint8_t next_sector[DISK_SECTOR_SIZE];
    err = cache_read(fat->cache, lba + 1u, next_sector);
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
                    ((uint32_t)index * FAT12_DIR_ENTRY_SIZE) /
                        DISK_SECTOR_SIZE);
}

static uint16_t fat12_root_offset(uint16_t index) {
  return (uint16_t)(((uint32_t)index * FAT12_DIR_ENTRY_SIZE) %
                    DISK_SECTOR_SIZE);
}

disk_err_t fat12_read_root_entry(fat12_t *fat, uint16_t index,
                                 fat12_dirent_t *entry) {
  if (fat == NULL || entry == NULL) return DISK_ERR_INVALID;
  if (index >= FAT12_ROOT_ENTRIES) return DISK_END;
  uint8_t sector[DISK_SECTOR_SIZE];
  disk_err_t err = cache_read(fat->cache, fat12_root_lba(index), sector);
  if (err != DISK_OK) return err;
  fat12_decode_dirent(entry, sector + fat12_root_offset(index));
  return DISK_OK;
}

bool fat12_entry_valid(const fat12_dirent_t *entry) {
  if (entry == NULL) return false;
  uint8_t first = (uint8_t)entry->name[0];
  return first != FAT12_DIRENT_END && first != FAT12_DIRENT_FREE &&
         entry->attr != FAT12_ATTR_LFN;
}

bool fat12_entry_is_end(const fat12_dirent_t *entry) {
  return entry != NULL && (uint8_t)entry->name[0] == FAT12_DIRENT_END;
}

static bool fat12_name_char_valid(char value) {
  if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
      (value >= '0' && value <= '9'))
    return true;
  switch (value) {
    case '$':
    case '%':
    case '\'':
    case '-':
    case '_':
    case '@':
    case '~':
    case '`':
    case '!':
    case '(':
    case ')':
    case '{':
    case '}':
    case '^':
    case '#':
    case '&':
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

disk_err_t fat12_root_lookup(fat12_t *fat, const fat12_name_t *name,
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
    if (!fat12_entry_valid(&entry) || (entry.attr & FAT12_ATTR_VOLUME_ID) != 0)
      continue;
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

disk_err_t fat12_set_entry(fat12_t *fat, uint16_t cluster, uint16_t value) {
  if (!fat12_cluster_valid(cluster) || value > 0x0FFFu) {
    return DISK_ERR_INVALID;
  }
  fat12_entry_loc_t loc = fat12_entry_locate(cluster);
  uint8_t sector[DISK_SECTOR_SIZE];
  uint8_t sector2[DISK_SECTOR_SIZE];
  disk_err_t err =
      cache_read(fat->cache, (uint16_t)(fat->fat_start + loc.sector), sector);
  if (err != DISK_OK) return err;
  if (loc.split) {
    err = cache_read(fat->cache, (uint16_t)(fat->fat_start + loc.sector + 1u),
                     sector2);
    if (err != DISK_OK) return err;
  }
  uint8_t *hi = loc.split ? &sector2[0] : &sector[loc.offset + 1u];
  fat12_entry_pack(cluster, value, &sector[loc.offset], hi);

  for (uint8_t copy = 0; copy < FAT12_NUM_FATS; copy++) {
    uint16_t lba =
        (uint16_t)(FAT12_RESERVED_SECTORS +
                   (uint16_t)copy * FAT12_SECTORS_PER_FAT + loc.sector);
    err = cache_write(fat->cache, lba, sector);
    if (err != DISK_OK) return err;
    if (loc.split) {
      err = cache_write(fat->cache, (uint16_t)(lba + 1u), sector2);
      if (err != DISK_OK) return err;
    }
  }
  return DISK_OK;
}

void fat12_scan_init(fat12_scan_t *scan, fat12_t *fat, uint16_t fat_start,
                     uint16_t start) {
  memset(scan, 0, sizeof(*scan));
  scan->fat = fat;
  scan->cluster = start;
  scan->fat_start = fat_start;
  scan->cached_lba = UINT16_MAX;
  scan->cached_lba2 = UINT16_MAX;
}

disk_err_t fat12_scan_next(fat12_scan_t *scan, uint16_t end, uint16_t *cluster,
                           uint16_t *entry) {
  if (scan->cluster >= end) return DISK_END;
  fat12_entry_loc_t loc = fat12_entry_locate(scan->cluster);
  uint16_t lba = (uint16_t)(scan->fat_start + loc.sector);
  if (lba != scan->cached_lba) {
    disk_err_t err = cache_read(scan->fat->cache, lba, scan->sector);
    if (err != DISK_OK) return err;
    scan->cached_lba = lba;
    scan->cached_lba2 = UINT16_MAX;
  }
  uint8_t hi;
  if (loc.split) {
    if (scan->cached_lba2 != lba + 1u) {
      disk_err_t err = cache_read(scan->fat->cache, lba + 1u, scan->sector2);
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

disk_err_t fat12_find_free_cluster(fat12_t *fat, uint16_t start,
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
  while ((err = fat12_scan_next(&scan, FAT12_CLUSTER_LIMIT, &cluster,
                                &entry)) == DISK_OK) {
    if (entry == 0) total++;
  }
  if (err != DISK_END) return err;
  *count = total;
  return DISK_OK;
}

disk_err_t fat12_write_root_entry(fat12_t *fat, uint16_t index,
                                  const fat12_dirent_t *entry) {
  if (index >= FAT12_ROOT_ENTRIES) return DISK_END;
  uint16_t lba = fat12_root_lba(index);
  uint8_t sector[DISK_SECTOR_SIZE];
  disk_err_t err = cache_read(fat->cache, lba, sector);
  if (err != DISK_OK) return err;
  fat12_encode_dirent(sector + fat12_root_offset(index), entry);
  return cache_write(fat->cache, lba, sector);
}
