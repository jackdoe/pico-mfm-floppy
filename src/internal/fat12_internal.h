#ifndef FAT12_INTERNAL_H
#define FAT12_INTERNAL_H

#include "fat12.h"

#define FAT12_EOC 0x0FFFu
#define FAT12_BAD 0x0FF7u

typedef struct {
  bool found;
  uint16_t index;
  fat12_dirent_t entry;
  uint16_t free_index;
  bool free_is_end;
} fat12_lookup_t;

typedef struct {
  fat12_t *fat;
  uint16_t cluster;
  uint16_t cached_lba;
  uint16_t cached_lba2;
  uint16_t fat_start;
  uint8_t sector[DISK_SECTOR_SIZE];
  uint8_t sector2[DISK_SECTOR_SIZE];
} fat12_scan_t;

static inline bool fat12_is_bad(uint16_t cluster) {
  return cluster == FAT12_BAD;
}

static inline bool fat12_cluster_valid(uint16_t cluster) {
  return cluster >= 2 && cluster < FAT12_CLUSTER_LIMIT;
}

static inline uint16_t fat12_cluster_to_lba(uint16_t cluster) {
  return FAT12_DATA_START + cluster - 2u;
}

static inline char fat12_toupper(char value) {
  return value >= 'a' && value <= 'z' ? (char)(value - ('a' - 'A')) : value;
}

void fat12_decode_dirent(fat12_dirent_t *entry, const uint8_t *raw);
void fat12_encode_dirent(uint8_t *raw, const fat12_dirent_t *entry);
disk_err_t fat12_resolve_entry(fat12_t *fat, uint16_t fat_start,
                               uint16_t cluster, uint16_t *next);
disk_err_t fat12_root_lookup(fat12_t *fat, const fat12_name_t *name,
                             fat12_lookup_t *out);
disk_err_t fat12_set_entry(fat12_t *fat, uint16_t cluster, uint16_t value);
void fat12_scan_init(fat12_scan_t *scan, fat12_t *fat, uint16_t fat_start,
                     uint16_t start);
disk_err_t fat12_scan_next(fat12_scan_t *scan, uint16_t end, uint16_t *cluster,
                           uint16_t *entry);
disk_err_t fat12_find_free_cluster(fat12_t *fat, uint16_t start, uint16_t *out);
disk_err_t fat12_write_root_entry(fat12_t *fat, uint16_t index,
                                  const fat12_dirent_t *entry);
void fat12_transaction_end(fat12_t *fat);
disk_err_t fat12_select_active_fat(fat12_t *fat);

#endif
