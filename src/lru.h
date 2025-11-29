#ifndef LRU_H
#define LRU_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

// ============== LRU Cache ==============
// Simple LRU cache with uint32_t keys and fixed-size pre-allocated values
// Uses doubly-linked list for LRU ordering and linear search for lookup
// (For small caches like sector caches, linear search is fine)
// All element storage is pre-allocated at init time - no per-entry malloc

typedef struct lru_entry {
  uint32_t key;
  struct lru_entry *prev;
  struct lru_entry *next;
  bool occupied;
  // Value data follows immediately after this struct (variable size)
} lru_entry_t;

typedef struct {
  uint8_t *storage;         // Pre-allocated storage for all entries
  lru_entry_t *head;        // Most recently used
  lru_entry_t *tail;        // Least recently used
  uint32_t max_entries;
  uint32_t elem_size;       // Size of each value element
  uint32_t entry_stride;    // Total size of each entry (header + value)
  uint32_t count;
} lru_t;

// ============== Public API ==============

// Initialize LRU cache with pre-allocated storage
// max_entries: maximum number of entries
// elem_size: size of each value element in bytes
// All storage is allocated upfront (no per-entry malloc)
lru_t *lru_init(uint32_t max_entries, uint32_t elem_size);

// Free the LRU cache
void lru_free(lru_t *lru);

// Get value by key (returns NULL if not found)
// Moves the entry to the front (most recently used)
// Returns pointer to the pre-allocated value storage
void *lru_get(lru_t *lru, uint32_t key);

// Set value for key (copies data into pre-allocated storage)
// If key exists, updates value and moves to front
// If cache is full, evicts least recently used entry
// Returns pointer to the stored value, or NULL on failure
void *lru_set(lru_t *lru, uint32_t key, const void *value);

// Get or create slot for key without copying data
// Useful when you want to write directly into the cache buffer
// Returns pointer to value storage (may contain stale data if newly allocated)
// Sets *is_new to true if this is a new entry, false if existing
void *lru_get_or_create(lru_t *lru, uint32_t key, bool *is_new);

// Remove entry by key
// Returns true if entry was found and removed, false otherwise
bool lru_remove(lru_t *lru, uint32_t key);

// Clear all entries
void lru_clear(lru_t *lru);

// Get current entry count
uint32_t lru_count(lru_t *lru);

// Get element size
uint32_t lru_elem_size(lru_t *lru);

// Helper to create cache key from track/side/sector
static inline uint32_t lru_key(int track, int side, int sector_n) {
  return (uint32_t)track << 16 | (uint32_t)side << 8 | (uint32_t)sector_n;
}

#endif // LRU_H
