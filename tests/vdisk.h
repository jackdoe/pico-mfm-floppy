#ifndef VDISK_H
#define VDISK_H

#include "../src/floppy.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define VDISK_TRACKS 80
#define VDISK_SIDES 2
#define VDISK_TOTAL_SECTORS (VDISK_TRACKS * VDISK_SIDES * SECTORS_PER_TRACK)

typedef struct {
  uint8_t data[VDISK_TOTAL_SECTORS][SECTOR_SIZE];
  int read_count;
  int write_count;
  int track_writes;
  bool write_protected;
  bool disk_changed;
} vdisk_t;

static inline int vdisk_lba(uint8_t track, uint8_t side, uint8_t sector_n) {
  return (track * VDISK_SIDES + side) * SECTORS_PER_TRACK + (sector_n - 1);
}

static inline void vdisk_init(vdisk_t *disk) {
  memset(disk, 0, sizeof(*disk));
}

static bool vdisk_read(void *ctx, sector_t *sector) {
  vdisk_t *disk = (vdisk_t *)ctx;
  int lba = vdisk_lba(sector->track, sector->side, sector->sector_n);
  if (lba < 0 || lba >= VDISK_TOTAL_SECTORS) {
    sector->valid = false;
    return false;
  }
  memcpy(sector->data, disk->data[lba], SECTOR_SIZE);
  sector->valid = true;
  sector->size_code = 2;
  disk->read_count++;
  return true;
}

static bool vdisk_write(void *ctx, track_t *track) {
  vdisk_t *disk = (vdisk_t *)ctx;
  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    if (!track->sectors[i].valid) {
      int lba = vdisk_lba(track->track, track->side, i + 1);
      if (lba >= 0 && lba < VDISK_TOTAL_SECTORS) {
        memcpy(track->sectors[i].data, disk->data[lba], SECTOR_SIZE);
        track->sectors[i].valid = true;
        track->sectors[i].track = track->track;
        track->sectors[i].side = track->side;
        track->sectors[i].sector_n = i + 1;
        track->sectors[i].size_code = 2;
        disk->read_count++;
      }
    }
  }
  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    int lba = vdisk_lba(track->track, track->side, i + 1);
    if (lba >= 0 && lba < VDISK_TOTAL_SECTORS) {
      memcpy(disk->data[lba], track->sectors[i].data, SECTOR_SIZE);
    }
  }
  disk->write_count += SECTORS_PER_TRACK;
  disk->track_writes++;
  return true;
}

static bool vdisk_disk_changed(void *ctx) {
  vdisk_t *disk = (vdisk_t *)ctx;
  if (disk->disk_changed) {
    disk->disk_changed = false;
    return true;
  }
  return false;
}

static bool vdisk_write_protected(void *ctx) {
  vdisk_t *disk = (vdisk_t *)ctx;
  return disk->write_protected;
}

static inline void vdisk_format_valid(vdisk_t *disk) {
  memset(disk, 0, sizeof(*disk));
  uint8_t *boot = disk->data[0];
  boot[0] = 0xEB; boot[1] = 0x3C; boot[2] = 0x90;
  memcpy(&boot[3], "MSDOS5.0", 8);
  boot[11] = SECTOR_SIZE & 0xFF;
  boot[12] = SECTOR_SIZE >> 8;
  boot[13] = 1;
  boot[14] = 1; boot[15] = 0;
  boot[16] = 2;
  boot[17] = 224; boot[18] = 0;
  boot[19] = (VDISK_TOTAL_SECTORS) & 0xFF;
  boot[20] = (VDISK_TOTAL_SECTORS) >> 8;
  boot[21] = 0xF0;
  boot[22] = 9; boot[23] = 0;
  boot[24] = 18; boot[25] = 0;
  boot[26] = 2; boot[27] = 0;
  boot[28] = 0; boot[29] = 0; boot[30] = 0; boot[31] = 0;
  boot[510] = 0x55;
  boot[511] = 0xAA;
  disk->data[1][0] = 0xF0;
  disk->data[1][1] = 0xFF;
  disk->data[1][2] = 0xFF;
  disk->data[10][0] = 0xF0;
  disk->data[10][1] = 0xFF;
  disk->data[10][2] = 0xFF;
}

#endif
