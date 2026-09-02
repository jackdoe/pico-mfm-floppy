#ifndef VDISK_H
#define VDISK_H

#include "../src/cache.h"
#include "../src/fat12.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  uint8_t data[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE];
  uint32_t generation;
  bool write_protected;
  disk_err_t read_status;
  uint32_t error_valid;
  bool corrupt_track;
  disk_err_t write_status;
  bool apply_failed_write;
  disk_err_t generation_status;
  disk_err_t write_protected_status;
  uint32_t generation_calls;
  uint32_t change_generation_on_call;
  int reads_before_failure;
  int writes_before_failure;
  disk_err_t read_failure;
  disk_err_t write_failure;
  int fail_track;
  bool fail_reads_after_write;
  bool tear_next_write;
  int writes_before_tear;
  int track_reads;
  int track_writes;
  int write_order[DISK_TRACK_COUNT];
  track_t last_write;
  uint32_t last_expected_generation;
} vdisk_t;

static inline void vdisk_init(vdisk_t *disk) {
  memset(disk, 0, sizeof(*disk));
  disk->generation = 1;
  disk->reads_before_failure = -1;
  disk->writes_before_failure = -1;
  disk->read_failure = DISK_ERR_TIMEOUT;
  disk->write_failure = DISK_ERR_VERIFY;
  disk->fail_track = -1;
}

static inline uint16_t vdisk_first_lba(uint8_t cylinder, uint8_t head) {
  return (uint16_t)(((uint16_t)cylinder * DISK_HEADS + head) * DISK_SECTORS_PER_TRACK);
}

static inline void vdisk_copy_track_out(const vdisk_t *disk, uint8_t cylinder,
                                        uint8_t head, track_t *out) {
  uint16_t first = vdisk_first_lba(cylinder, head);
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    memcpy(out->data[sector], disk->data[first + sector], DISK_SECTOR_SIZE);
  }
}

static inline void vdisk_copy_track_in(vdisk_t *disk, const track_t *track,
                                       uint8_t sectors) {
  uint16_t first = vdisk_first_lba(track->cylinder, track->head);
  for (uint8_t sector = 0; sector < sectors; sector++) {
    memcpy(disk->data[first + sector], track->data[sector], DISK_SECTOR_SIZE);
  }
}

static inline disk_err_t vdisk_read_track(void *ctx, uint32_t expected_generation,
                                          uint8_t cylinder, uint8_t head,
                                          track_t *out) {
  vdisk_t *disk = ctx;
  if (!disk || !out || !disk_ch_valid(cylinder, head)) return DISK_ERR_INVALID;
  if (expected_generation != disk->generation) return DISK_ERR_MEDIA_CHANGED;
  int track = cylinder * (int)DISK_HEADS + head;
  if (disk->fail_reads_after_write && disk->track_writes != 0) {
    return disk->read_failure;
  }
  if (disk->fail_track == track) return disk->read_failure;
  if (disk->reads_before_failure == 0) return disk->read_failure;
  if (disk->reads_before_failure > 0) disk->reads_before_failure--;
  memset(out, 0, sizeof(*out));
  out->cylinder = cylinder;
  out->head = head;
  vdisk_copy_track_out(disk, cylinder, head, out);
  out->valid = disk->read_status == DISK_OK
      ? (disk->corrupt_track ? DISK_TRACK_VALID ^ 1u : DISK_TRACK_VALID)
      : disk->error_valid;
  disk->track_reads++;
  return disk->read_status;
}

static inline disk_err_t vdisk_write_track(void *ctx, uint32_t expected_generation,
                                           const track_t *track) {
  vdisk_t *disk = ctx;
  if (!disk || !track || !disk_ch_valid(track->cylinder, track->head) ||
      track->valid != DISK_TRACK_VALID) {
    return DISK_ERR_INVALID;
  }
  disk->last_expected_generation = expected_generation;
  if (expected_generation != disk->generation) return DISK_ERR_MEDIA_CHANGED;
  if (disk->write_protected) return DISK_ERR_WRITE_PROTECTED;
  if (disk->tear_next_write) {
    if (disk->writes_before_tear > 0) {
      disk->writes_before_tear--;
    } else {
      disk->tear_next_write = false;
      vdisk_copy_track_in(disk, track, DISK_SECTORS_PER_TRACK / 2u);
      return DISK_ERR_IO;
    }
  }
  if (disk->writes_before_failure == 0) {
    if (disk->apply_failed_write) vdisk_copy_track_in(disk, track, DISK_SECTORS_PER_TRACK);
    return disk->write_failure;
  }
  if (disk->writes_before_failure > 0) disk->writes_before_failure--;
  if (disk->write_status == DISK_OK || disk->apply_failed_write) {
    vdisk_copy_track_in(disk, track, DISK_SECTORS_PER_TRACK);
  }
  if (disk->write_status != DISK_OK) return disk->write_status;
  disk->last_write = *track;
  if (disk->track_writes < (int)DISK_TRACK_COUNT) {
    disk->write_order[disk->track_writes] =
        track->cylinder * (int)DISK_HEADS + track->head;
  }
  disk->track_writes++;
  return DISK_OK;
}

static inline disk_err_t vdisk_media_generation(void *ctx, uint32_t *generation) {
  vdisk_t *disk = ctx;
  if (!disk || !generation) return DISK_ERR_INVALID;
  disk->generation_calls++;
  if (disk->generation_status != DISK_OK) return disk->generation_status;
  if (disk->change_generation_on_call == disk->generation_calls) disk->generation++;
  *generation = disk->generation;
  return DISK_OK;
}

static inline disk_err_t vdisk_write_protected(void *ctx, bool *write_protected) {
  vdisk_t *disk = ctx;
  if (!disk || !write_protected) return DISK_ERR_INVALID;
  if (disk->write_protected_status != DISK_OK) return disk->write_protected_status;
  *write_protected = disk->write_protected;
  return DISK_OK;
}

static inline disk_device_t vdisk_device(vdisk_t *disk) {
  return (disk_device_t){
      .read_track = vdisk_read_track,
      .write_track = vdisk_write_track,
      .media_generation = vdisk_media_generation,
      .write_protected = vdisk_write_protected,
      .ctx = disk,
  };
}

static inline disk_device_t vdisk_readonly_device(vdisk_t *disk) {
  disk_device_t device = vdisk_device(disk);
  device.write_track = NULL;
  return device;
}

static inline void vdisk_format_valid(vdisk_t *disk) {
  vdisk_init(disk);
  uint8_t *boot = disk->data[0];
  boot[0] = 0xEB;
  boot[1] = 0x3C;
  boot[2] = 0x90;
  memcpy(boot + 3, "MSDOS5.0", 8);
  boot[11] = (uint8_t)DISK_SECTOR_SIZE;
  boot[12] = (uint8_t)(DISK_SECTOR_SIZE >> 8);
  boot[13] = FAT12_SECTORS_PER_CLUSTER;
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
  fat12_entry_loc_t loc = fat12_entry_locate(cluster);
  uint8_t *lo = &disk->data[fat_start + loc.sector][loc.offset];
  uint8_t *hi = loc.split ? &disk->data[fat_start + loc.sector + 1u][0] : lo + 1;
  fat12_entry_pack(cluster, value, lo, hi);
}

static inline void vdisk_set_fat_entry(vdisk_t *disk, uint16_t cluster,
                                       uint16_t value) {
  vdisk_set_fat_copy_entry(disk, FAT12_FAT1_START, cluster, value);
  vdisk_set_fat_copy_entry(disk, FAT12_FAT2_START, cluster, value);
}

static inline uint16_t vdisk_get_fat_copy_entry(const vdisk_t *disk,
                                                uint16_t fat_start,
                                                uint16_t cluster) {
  fat12_entry_loc_t loc = fat12_entry_locate(cluster);
  uint8_t lo = disk->data[fat_start + loc.sector][loc.offset];
  uint8_t hi = loc.split ? disk->data[fat_start + loc.sector + 1u][0]
                         : disk->data[fat_start + loc.sector][loc.offset + 1u];
  return fat12_entry_unpack(cluster, lo, hi);
}

#endif
