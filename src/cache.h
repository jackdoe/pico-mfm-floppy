#ifndef CACHE_H
#define CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include "disk.h"

#ifndef CACHE_TRACKS
#define CACHE_TRACKS 6u
#endif

typedef struct {
  track_t track;
  uint32_t dirty;
  uint32_t conflicted;
  uint64_t used;
  bool occupied;
} cache_slot_t;

typedef struct {
  disk_device_t device;
  cache_slot_t slots[CACHE_TRACKS];
  track_t work;
  uint64_t clock;
  uint32_t generation;
} cache_t;

disk_err_t cache_init(cache_t *cache, disk_device_t device);
disk_err_t cache_bind(cache_t *cache);
disk_err_t cache_check(cache_t *cache);
disk_err_t cache_writable(cache_t *cache);
void cache_clear(cache_t *cache);
bool cache_dirty(const cache_t *cache);
disk_err_t cache_read(cache_t *cache, uint16_t lba, uint8_t out[DISK_SECTOR_SIZE]);
disk_err_t cache_write(cache_t *cache, uint16_t lba,
                       const uint8_t data[DISK_SECTOR_SIZE]);
disk_err_t cache_flush(cache_t *cache);
void cache_discard(cache_t *cache);

#endif
