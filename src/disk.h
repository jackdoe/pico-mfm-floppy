#ifndef DISK_H
#define DISK_H

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
  DISK_OK = 0,
  DISK_END,
  DISK_ERR_INVALID,
  DISK_ERR_BUSY,
  DISK_ERR_TIMEOUT,
  DISK_ERR_CRC,
  DISK_ERR_WRONG_TRACK,
  DISK_ERR_WRONG_SIDE,
  DISK_ERR_NO_TRACK0,
  DISK_ERR_MEDIA_CHANGED,
  DISK_ERR_WRITE_PROTECTED,
  DISK_ERR_UNDERRUN,
  DISK_ERR_OVERRUN,
  DISK_ERR_VERIFY,
  DISK_ERR_CORRUPT,
  DISK_ERR_IO,
  DISK_ERR_NOT_FOUND,
  DISK_ERR_EXISTS,
  DISK_ERR_FULL,
  DISK_ERR_READ_ONLY,
  DISK_ERR_AMBIGUOUS,
  DISK_ERR_NOT_INITIALIZED,
  DISK_ERR_NOT_MOUNTED,
  DISK_ERR_ALREADY_MOUNTED,
  DISK_ERR_TOO_MANY,
  DISK_ERR_IS_DIR,
  DISK_ERR_NOT_DIR,
  DISK_ERR_CONFLICT,
  DISK_ERR_BAD_HANDLE,
} disk_err_t;

#define DISK_ERR_LAST DISK_ERR_BAD_HANDLE

typedef struct {
  disk_err_t error;
  size_t count;
} disk_result_t;

typedef struct {
  uint8_t cylinder;
  uint8_t head;
  uint32_t valid;
  uint8_t data[DISK_SECTORS_PER_TRACK][DISK_SECTOR_SIZE];
} track_t;

typedef struct {
  disk_err_t (*read_track)(void *ctx, uint32_t expected_generation,
                           uint8_t cylinder, uint8_t head, track_t *out);
  disk_err_t (*write_track)(void *ctx, uint32_t expected_generation,
                            const track_t *track);
  disk_err_t (*media_generation)(void *ctx, uint32_t *generation);
  disk_err_t (*write_protected)(void *ctx, bool *write_protected);
  void *ctx;
} disk_device_t;

const char *disk_strerror(disk_err_t error);

static inline bool disk_err_is_io(disk_err_t error) {
  return error >= DISK_ERR_TIMEOUT && error <= DISK_ERR_IO;
}

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
