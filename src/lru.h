#ifndef LRU_H
#define LRU_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct lru_entry {
  uint32_t key;
  struct lru_entry *prev;
  struct lru_entry *next;
  bool occupied;
} lru_entry_t;

typedef struct {
  uint8_t *storage;
  lru_entry_t *head;
  lru_entry_t *tail;
  uint32_t max_entries;
  uint32_t elem_size;
  uint32_t entry_stride;
  uint32_t count;
} lru_t;

lru_t *lru_init(uint32_t max_entries, uint32_t elem_size);

void lru_free(lru_t *lru);

void *lru_get(lru_t *lru, uint32_t key);

void *lru_set(lru_t *lru, uint32_t key, const void *value);

void *lru_get_or_create(lru_t *lru, uint32_t key, bool *is_new);

bool lru_remove(lru_t *lru, uint32_t key);

void lru_clear(lru_t *lru);

uint32_t lru_count(lru_t *lru);

uint32_t lru_elem_size(lru_t *lru);

static inline uint32_t lru_key(int track, int side, int sector_n) {
  return (uint32_t)track << 16 | (uint32_t)side << 8 | (uint32_t)sector_n;
}

#endif
