#include "cache.h"
#include <string.h>

static uint64_t cache_tick(cache_t *cache) {
  cache->clock++;
  if (cache->clock != 0) return cache->clock;
  for (size_t i = 0; i < CACHE_TRACKS; i++) cache->slots[i].used = 0;
  cache->clock = 1;
  return cache->clock;
}

static void cache_touch(cache_t *cache, cache_slot_t *slot) {
  slot->used = cache_tick(cache);
}

static bool cache_slot_is(const cache_slot_t *slot, uint8_t cylinder, uint8_t head) {
  return slot->occupied && slot->track.cylinder == cylinder &&
         slot->track.head == head;
}

static uint16_t cache_slot_index(const cache_slot_t *slot) {
  return (uint16_t)(slot->track.cylinder * DISK_HEADS + slot->track.head);
}

void cache_clear(cache_t *cache) {
  if (!cache) return;
  memset(cache->slots, 0, sizeof(cache->slots));
  cache->clock = 0;
}

disk_err_t cache_init(cache_t *cache, disk_device_t device) {
  if (!cache || !device.read_track || !device.media_generation ||
      !device.write_protected) {
    return DISK_ERR_INVALID;
  }
  memset(cache, 0, sizeof(*cache));
  cache->device = device;
  return DISK_OK;
}

disk_err_t cache_bind(cache_t *cache) {
  if (!cache) return DISK_ERR_INVALID;
  uint32_t generation;
  disk_err_t status = cache->device.media_generation(cache->device.ctx, &generation);
  if (status != DISK_OK) return status;
  cache_clear(cache);
  cache->generation = generation;
  return DISK_OK;
}

disk_err_t cache_check(cache_t *cache) {
  if (!cache) return DISK_ERR_INVALID;
  uint32_t generation;
  disk_err_t status = cache->device.media_generation(cache->device.ctx, &generation);
  if (status != DISK_OK) {
    if (status == DISK_ERR_MEDIA_CHANGED) cache_clear(cache);
    return status;
  }
  if (generation != cache->generation) {
    cache_clear(cache);
    return DISK_ERR_MEDIA_CHANGED;
  }
  return DISK_OK;
}

disk_err_t cache_writable(cache_t *cache) {
  if (!cache) return DISK_ERR_INVALID;
  if (!cache->device.write_track) return DISK_ERR_WRITE_PROTECTED;
  bool write_protected;
  disk_err_t status =
      cache->device.write_protected(cache->device.ctx, &write_protected);
  if (status != DISK_OK) return status;
  return write_protected ? DISK_ERR_WRITE_PROTECTED : DISK_OK;
}

bool cache_dirty(const cache_t *cache) {
  if (!cache) return false;
  for (size_t i = 0; i < CACHE_TRACKS; i++) {
    if (cache->slots[i].dirty) return true;
  }
  return false;
}

static disk_err_t cache_merge(cache_slot_t *slot, const track_t *fresh,
                              bool authoritative) {
  disk_err_t status = DISK_OK;
  if (authoritative) slot->conflicted = 0;
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    uint32_t bit = 1u << sector;
    if (!track_has(fresh, sector) || (slot->dirty & bit)) continue;
    if (authoritative) {
      memcpy(slot->track.data[sector], fresh->data[sector], DISK_SECTOR_SIZE);
      slot->track.valid |= bit;
    } else if (slot->conflicted & bit) {
      status = DISK_ERR_CORRUPT;
    } else if (track_has(&slot->track, sector)) {
      if (memcmp(slot->track.data[sector], fresh->data[sector],
                 DISK_SECTOR_SIZE) != 0) {
        slot->track.valid &= ~bit;
        slot->conflicted |= bit;
        status = DISK_ERR_CORRUPT;
      }
    } else {
      memcpy(slot->track.data[sector], fresh->data[sector], DISK_SECTOR_SIZE);
      slot->track.valid |= bit;
    }
  }
  return status;
}

static disk_err_t cache_refresh(cache_t *cache, cache_slot_t *slot) {
  uint8_t cylinder = slot->track.cylinder;
  uint8_t head = slot->track.head;
  memset(&cache->work, 0, sizeof(cache->work));
  cache->work.cylinder = cylinder;
  cache->work.head = head;
  disk_err_t status = cache->device.read_track(
      cache->device.ctx, cache->generation, cylinder, head, &cache->work);
  disk_err_t media = cache_check(cache);
  if (media != DISK_OK) return media;
  if (cache->work.cylinder != cylinder || cache->work.head != head ||
      (cache->work.valid & ~DISK_TRACK_VALID) != 0) {
    return status == DISK_OK ? DISK_ERR_CORRUPT : status;
  }
  bool authoritative = status == DISK_OK && cache->work.valid == DISK_TRACK_VALID;
  disk_err_t merged = cache_merge(slot, &cache->work, authoritative);
  return merged != DISK_OK ? merged : status;
}

static disk_err_t cache_flush_slot(cache_t *cache, cache_slot_t *slot) {
  if (!slot->dirty) return DISK_OK;
  disk_err_t status = cache_writable(cache);
  if (status != DISK_OK) return status;
  if (slot->track.valid != DISK_TRACK_VALID) {
    status = cache_refresh(cache, slot);
    if (slot->track.valid != DISK_TRACK_VALID) {
      return status == DISK_OK ? DISK_ERR_CORRUPT : status;
    }
  }
  status = cache->device.write_track(cache->device.ctx, cache->generation,
                                     &slot->track);
  if (status != DISK_OK) {
    if (status == DISK_ERR_MEDIA_CHANGED) cache_clear(cache);
    return status;
  }
  slot->dirty = 0;
  slot->conflicted = 0;
  return cache_check(cache);
}

static disk_err_t cache_acquire(cache_t *cache, uint8_t cylinder, uint8_t head,
                                cache_slot_t **out) {
  cache_slot_t *victim = NULL;
  cache_slot_t *clean = NULL;
  cache_slot_t *empty = NULL;
  for (size_t i = 0; i < CACHE_TRACKS; i++) {
    cache_slot_t *slot = &cache->slots[i];
    if (cache_slot_is(slot, cylinder, head)) {
      cache_touch(cache, slot);
      *out = slot;
      return DISK_OK;
    }
    if (!slot->occupied) {
      if (!empty) empty = slot;
    } else if (!slot->dirty) {
      if (!clean || slot->used < clean->used) clean = slot;
    } else if (!victim || slot->used < victim->used) {
      victim = slot;
    }
  }
  cache_slot_t *slot = empty ? empty : clean ? clean : victim;
  if (slot->dirty) {
    disk_err_t status = cache_flush_slot(cache, slot);
    if (status != DISK_OK) return status;
  }
  memset(slot, 0, sizeof(*slot));
  slot->track.cylinder = cylinder;
  slot->track.head = head;
  slot->occupied = true;
  cache_touch(cache, slot);
  *out = slot;
  return DISK_OK;
}

disk_err_t cache_read(cache_t *cache, uint16_t lba, uint8_t out[DISK_SECTOR_SIZE]) {
  if (!cache || !out) return DISK_ERR_INVALID;
  uint8_t cylinder;
  uint8_t head;
  uint8_t sector;
  if (!disk_lba_to_chs(lba, &cylinder, &head, &sector)) return DISK_ERR_INVALID;
  disk_err_t status = cache_check(cache);
  if (status != DISK_OK) return status;
  cache_slot_t *slot;
  status = cache_acquire(cache, cylinder, head, &slot);
  if (status != DISK_OK) return status;
  if (!track_has(&slot->track, sector)) {
    status = cache_refresh(cache, slot);
    if (!cache_slot_is(slot, cylinder, head) || !track_has(&slot->track, sector)) {
      return status == DISK_OK ? DISK_ERR_CORRUPT : status;
    }
  }
  memcpy(out, slot->track.data[sector], DISK_SECTOR_SIZE);
  return cache_check(cache);
}

disk_err_t cache_write(cache_t *cache, uint16_t lba,
                       const uint8_t data[DISK_SECTOR_SIZE]) {
  if (!cache || !data) return DISK_ERR_INVALID;
  uint8_t cylinder;
  uint8_t head;
  uint8_t sector;
  if (!disk_lba_to_chs(lba, &cylinder, &head, &sector)) return DISK_ERR_INVALID;
  if (!cache->device.write_track) return DISK_ERR_WRITE_PROTECTED;
  disk_err_t status = cache_check(cache);
  if (status != DISK_OK) return status;
  cache_slot_t *slot;
  status = cache_acquire(cache, cylinder, head, &slot);
  if (status != DISK_OK) return status;
  uint32_t bit = 1u << sector;
  memcpy(slot->track.data[sector], data, DISK_SECTOR_SIZE);
  slot->track.valid |= bit;
  slot->dirty |= bit;
  slot->conflicted &= ~bit;
  return DISK_OK;
}

disk_err_t cache_flush(cache_t *cache) {
  if (!cache) return DISK_ERR_INVALID;
  disk_err_t status = cache_check(cache);
  if (status != DISK_OK) return status;
  for (;;) {
    cache_slot_t *next = NULL;
    for (size_t i = 0; i < CACHE_TRACKS; i++) {
      cache_slot_t *slot = &cache->slots[i];
      if (!slot->dirty) continue;
      if (!next || cache_slot_index(slot) < cache_slot_index(next)) next = slot;
    }
    if (!next) return DISK_OK;
    status = cache_flush_slot(cache, next);
    if (status != DISK_OK) return status;
  }
}

void cache_discard(cache_t *cache) {
  if (!cache) return;
  for (size_t i = 0; i < CACHE_TRACKS; i++) {
    cache_slot_t *slot = &cache->slots[i];
    slot->track.valid &= ~slot->dirty;
    slot->dirty = 0;
  }
}
