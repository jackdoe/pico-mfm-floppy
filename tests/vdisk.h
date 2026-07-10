#ifndef VDISK_H
#define VDISK_H

#include "../src/fat12.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  uint8_t data[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE];
  int read_count;
  int write_count;
  int track_writes;
  int cyl_writes[DISK_CYLINDERS];
  bool write_protected;
  bool disk_changed;
} vdisk_t;

static inline int vdisk_lba(uint8_t cylinder, uint8_t head, uint8_t sector) {
  uint16_t lba;
  if (!disk_chs_to_lba(cylinder, head, sector, &lba)) return -1;
  return (int)lba;
}

static inline void vdisk_init(vdisk_t *disk) {
  memset(disk, 0, sizeof(*disk));
}

static block_status_t vdisk_read(
    void *ctx, uint16_t lba, uint8_t out[DISK_SECTOR_SIZE]) {
  vdisk_t *disk = ctx;
  if (disk == NULL || out == NULL || lba >= DISK_SECTOR_COUNT) {
    return BLOCK_ERR_INVALID;
  }
  if (disk->disk_changed) return BLOCK_ERR_MEDIA_CHANGED;
  memcpy(out, disk->data[lba], DISK_SECTOR_SIZE);
  disk->read_count++;
  return BLOCK_OK;
}

static block_status_t vdisk_write(void *ctx, const track_t *track) {
  vdisk_t *disk = ctx;
  if (disk == NULL || track == NULL ||
      !disk_ch_valid(track->cylinder, track->head) ||
      (track->valid & ~DISK_TRACK_VALID) != 0) {
    return BLOCK_ERR_INVALID;
  }
  if (disk->disk_changed) return BLOCK_ERR_MEDIA_CHANGED;
  if (disk->write_protected) return BLOCK_ERR_WRITE_PROTECTED;
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    if (!track_has(track, sector)) continue;
    uint16_t lba;
    if (!disk_chs_to_lba(track->cylinder, track->head, sector, &lba)) {
      return BLOCK_ERR_INVALID;
    }
    memcpy(disk->data[lba], track->data[sector], DISK_SECTOR_SIZE);
    disk->write_count++;
  }
  disk->track_writes++;
  disk->cyl_writes[track->cylinder]++;
  return BLOCK_OK;
}

static inline void vdisk_format_valid(vdisk_t *disk) {
  memset(disk, 0, sizeof(*disk));
  uint8_t *boot = disk->data[0];
  boot[0] = 0xEB;
  boot[1] = 0x3C;
  boot[2] = 0x90;
  memcpy(boot + 3, "MSDOS5.0", 8);
  boot[11] = (uint8_t)DISK_SECTOR_SIZE;
  boot[12] = (uint8_t)(DISK_SECTOR_SIZE >> 8);
  boot[13] = FAT12_MAX_CLUSTER_SECTORS;
  boot[14] = (uint8_t)FAT12_RESERVED_SECTORS;
  boot[15] = (uint8_t)(FAT12_RESERVED_SECTORS >> 8u);
  boot[16] = FAT12_NUM_FATS;
  boot[17] = (uint8_t)FAT12_ROOT_ENTRIES;
  boot[18] = (uint8_t)(FAT12_ROOT_ENTRIES >> 8u);
  boot[19] = (uint8_t)DISK_SECTOR_COUNT;
  boot[20] = (uint8_t)(DISK_SECTOR_COUNT >> 8u);
  boot[21] = FAT12_MEDIA_DESCRIPTOR;
  boot[22] = (uint8_t)FAT12_SECTORS_PER_FAT;
  boot[23] = (uint8_t)(FAT12_SECTORS_PER_FAT >> 8u);
  boot[24] = DISK_SECTORS_PER_TRACK;
  boot[26] = DISK_HEADS;
  boot[FAT12_BOOT_SIG_OFFSET] = 0x55;
  boot[FAT12_BOOT_SIG_OFFSET + 1u] = 0xAA;
  disk->data[FAT12_FAT1_START][0] = FAT12_MEDIA_DESCRIPTOR;
  disk->data[FAT12_FAT1_START][1] = 0xFF;
  disk->data[FAT12_FAT1_START][2] = 0xFF;
  disk->data[FAT12_FAT2_START][0] = FAT12_MEDIA_DESCRIPTOR;
  disk->data[FAT12_FAT2_START][1] = 0xFF;
  disk->data[FAT12_FAT2_START][2] = 0xFF;
}

static inline void vdisk_set_fat_copy_entry(vdisk_t *disk, uint16_t fat_start,
                                            uint16_t cluster,
                                            uint16_t value) {
  uint32_t fat_offset = (uint32_t)cluster + cluster / 2u;
  uint16_t lba = (uint16_t)(fat_start + fat_offset / DISK_SECTOR_SIZE);
  uint16_t offset = fat_offset % DISK_SECTOR_SIZE;
  uint8_t *first = disk->data[lba];
  uint8_t *second = offset == DISK_SECTOR_SIZE - 1u
      ? disk->data[lba + 1u]
      : first;
  uint8_t *lo = &first[offset];
  uint8_t *hi = offset == DISK_SECTOR_SIZE - 1u
      ? &second[0]
      : &second[offset + 1u];
  value &= 0x0FFFu;
  if (cluster & 1u) {
    *lo = (uint8_t)((*lo & 0x0Fu) | ((value & 0x0Fu) << 4));
    *hi = (uint8_t)(value >> 4);
  } else {
    *lo = (uint8_t)value;
    *hi = (uint8_t)((*hi & 0xF0u) | ((value >> 8) & 0x0Fu));
  }
}

static inline void vdisk_set_fat_entry(vdisk_t *disk, uint16_t cluster,
                                       uint16_t value) {
  vdisk_set_fat_copy_entry(disk, FAT12_FAT1_START, cluster, value);
  vdisk_set_fat_copy_entry(disk, FAT12_FAT2_START, cluster, value);
}

static inline uint16_t vdisk_get_fat_copy_entry(const vdisk_t *disk,
                                                uint16_t fat_start,
                                                uint16_t cluster) {
  uint32_t fat_offset = (uint32_t)cluster + cluster / 2u;
  uint16_t lba = (uint16_t)(fat_start + fat_offset / DISK_SECTOR_SIZE);
  uint16_t offset = fat_offset % DISK_SECTOR_SIZE;
  uint8_t lo = disk->data[lba][offset];
  uint8_t hi = offset == DISK_SECTOR_SIZE - 1u
      ? disk->data[lba + 1u][0]
      : disk->data[lba][offset + 1u];
  uint16_t value = (uint16_t)((uint16_t)lo |
                              (uint16_t)((uint16_t)hi << 8));
  return (cluster & 1u) != 0 ? value >> 4 : value & 0x0FFFu;
}

#endif
