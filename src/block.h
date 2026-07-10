#ifndef BLOCK_H
#define BLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISK_SECTOR_SIZE 512u
#define DISK_CYLINDERS 80u
#define DISK_HEADS 2u
#define DISK_SECTORS_PER_TRACK 18u
#define DISK_TRACK_COUNT (DISK_CYLINDERS * DISK_HEADS)
#define DISK_SECTOR_COUNT (DISK_TRACK_COUNT * DISK_SECTORS_PER_TRACK)
#define DISK_TRACK_VALID ((1u << DISK_SECTORS_PER_TRACK) - 1u)

_Static_assert(DISK_SECTORS_PER_TRACK < 32u, "track validity must fit uint32_t");
_Static_assert(DISK_SECTOR_COUNT <= UINT16_MAX, "LBA must fit uint16_t");

typedef enum {
  BLOCK_OK = 0,
  BLOCK_ERR_INVALID,
  BLOCK_ERR_BUSY,
  BLOCK_ERR_TIMEOUT,
  BLOCK_ERR_CRC,
  BLOCK_ERR_WRONG_TRACK,
  BLOCK_ERR_WRONG_SIDE,
  BLOCK_ERR_NO_TRACK0,
  BLOCK_ERR_MEDIA_CHANGED,
  BLOCK_ERR_WRITE_PROTECTED,
  BLOCK_ERR_UNDERRUN,
  BLOCK_ERR_OVERRUN,
  BLOCK_ERR_VERIFY,
  BLOCK_ERR_CORRUPT,
  BLOCK_ERR_IO,
} block_status_t;

typedef struct {
  uint8_t cylinder;
  uint8_t head;
  uint32_t valid;
  uint8_t data[DISK_SECTORS_PER_TRACK][DISK_SECTOR_SIZE];
} track_t;

typedef struct {
  block_status_t (*read_track)(void *ctx, uint32_t expected_generation,
                               uint8_t cylinder, uint8_t head, track_t *out);
  block_status_t (*write_track)(void *ctx, uint32_t expected_generation,
                                const track_t *track);
  block_status_t (*media_generation)(void *ctx, uint32_t *generation);
  block_status_t (*write_protected)(void *ctx, bool *write_protected);
  void *ctx;
} block_device_t;

static inline bool disk_ch_valid(uint8_t cylinder, uint8_t head) {
  return cylinder < DISK_CYLINDERS && head < DISK_HEADS;
}

static inline bool disk_sector_valid(uint8_t sector) {
  return sector < DISK_SECTORS_PER_TRACK;
}

static inline bool disk_chs_to_lba(uint8_t cylinder, uint8_t head,
                                   uint8_t sector, uint16_t *lba) {
  if (!lba || !disk_ch_valid(cylinder, head) ||
      !disk_sector_valid(sector)) {
    return false;
  }
  *lba = (uint16_t)(((uint16_t)cylinder * DISK_HEADS + head) *
                    DISK_SECTORS_PER_TRACK + sector);
  return true;
}

static inline bool disk_lba_to_chs(uint16_t lba, uint8_t *cylinder, uint8_t *head,
                                   uint8_t *sector) {
  if (lba >= DISK_SECTOR_COUNT || !cylinder || !head || !sector) return false;
  uint16_t track = lba / DISK_SECTORS_PER_TRACK;
  *cylinder = (uint8_t)(track / DISK_HEADS);
  *head = (uint8_t)(track % DISK_HEADS);
  *sector = (uint8_t)(lba % DISK_SECTORS_PER_TRACK);
  return true;
}

static inline bool disk_ch_to_track(uint8_t cylinder, uint8_t head,
                                    uint16_t *track) {
  if (!track || !disk_ch_valid(cylinder, head)) return false;
  *track = (uint16_t)((uint16_t)cylinder * DISK_HEADS + head);
  return true;
}

static inline bool track_has(const track_t *track, uint8_t sector) {
  return track && disk_sector_valid(sector) &&
         (track->valid & (1u << sector)) != 0;
}

static inline bool track_mark(track_t *track, uint8_t sector) {
  if (!track || !disk_sector_valid(sector)) return false;
  track->valid |= 1u << sector;
  return true;
}

#endif
