#include "lru.h"
#include <string.h>

// ============== Internal helpers ==============

// Get entry at index
static lru_entry_t *lru_entry_at(lru_t *lru, uint32_t index) {
  return (lru_entry_t *)(lru->storage + index * lru->entry_stride);
}

// Get value pointer for an entry
static void *lru_entry_value(lru_entry_t *entry) {
  return (uint8_t *)entry + sizeof(lru_entry_t);
}

// Remove entry from the linked list (doesn't free or clear it)
static void lru_unlink(lru_t *lru, lru_entry_t *entry) {
  if (entry->prev) {
    entry->prev->next = entry->next;
  } else {
    lru->head = entry->next;
  }
  if (entry->next) {
    entry->next->prev = entry->prev;
  } else {
    lru->tail = entry->prev;
  }
  entry->prev = NULL;
  entry->next = NULL;
}

// Add entry to the front (most recently used)
static void lru_push_front(lru_t *lru, lru_entry_t *entry) {
  entry->prev = NULL;
  entry->next = lru->head;
  if (lru->head) {
    lru->head->prev = entry;
  }
  lru->head = entry;
  if (!lru->tail) {
    lru->tail = entry;
  }
}

// Find entry by key (returns NULL if not found)
static lru_entry_t *lru_find(lru_t *lru, uint32_t key) {
  for (uint32_t i = 0; i < lru->max_entries; i++) {
    lru_entry_t *entry = lru_entry_at(lru, i);
    if (entry->occupied && entry->key == key) {
      return entry;
    }
  }
  return NULL;
}

// Find a free slot (returns NULL if cache is full)
static lru_entry_t *lru_find_free(lru_t *lru) {
  for (uint32_t i = 0; i < lru->max_entries; i++) {
    lru_entry_t *entry = lru_entry_at(lru, i);
    if (!entry->occupied) {
      return entry;
    }
  }
  return NULL;
}

// ============== Public API ==============

lru_t *lru_init(uint32_t max_entries, uint32_t elem_size) {
  if (max_entries == 0 || elem_size == 0) return NULL;

  lru_t *lru = (lru_t *)malloc(sizeof(lru_t));
  if (!lru) return NULL;

  // Calculate stride (entry header + value, aligned)
  uint32_t entry_stride = sizeof(lru_entry_t) + elem_size;
  // Align to 8 bytes for better performance
  entry_stride = (entry_stride + 7) & ~7u;

  lru->storage = (uint8_t *)calloc(max_entries, entry_stride);
  if (!lru->storage) {
    free(lru);
    return NULL;
  }

  lru->head = NULL;
  lru->tail = NULL;
  lru->max_entries = max_entries;
  lru->elem_size = elem_size;
  lru->entry_stride = entry_stride;
  lru->count = 0;

  return lru;
}

void lru_free(lru_t *lru) {
  if (!lru) return;
  free(lru->storage);
  free(lru);
}

void *lru_get(lru_t *lru, uint32_t key) {
  if (!lru) return NULL;

  lru_entry_t *entry = lru_find(lru, key);
  if (!entry) return NULL;

  // Move to front (most recently used)
  if (entry != lru->head) {
    lru_unlink(lru, entry);
    lru_push_front(lru, entry);
  }

  return lru_entry_value(entry);
}

void *lru_set(lru_t *lru, uint32_t key, const void *value) {
  if (!lru) return NULL;

  // Check if key already exists
  lru_entry_t *entry = lru_find(lru, key);
  if (entry) {
    // Update existing entry
    void *dest = lru_entry_value(entry);
    if (value) {
      memcpy(dest, value, lru->elem_size);
    }

    // Move to front
    if (entry != lru->head) {
      lru_unlink(lru, entry);
      lru_push_front(lru, entry);
    }
    return dest;
  }

  // Need a new slot
  entry = lru_find_free(lru);
  if (!entry) {
    // Cache full, evict LRU (tail)
    entry = lru->tail;
    if (!entry) return NULL;  // Shouldn't happen

    lru_unlink(lru, entry);
    lru->count--;
  }

  // Set up new entry
  entry->key = key;
  entry->occupied = true;
  void *dest = lru_entry_value(entry);
  if (value) {
    memcpy(dest, value, lru->elem_size);
  } else {
    memset(dest, 0, lru->elem_size);
  }
  lru_push_front(lru, entry);
  lru->count++;

  return dest;
}

void *lru_get_or_create(lru_t *lru, uint32_t key, bool *is_new) {
  if (!lru) return NULL;

  // Check if key already exists
  lru_entry_t *entry = lru_find(lru, key);
  if (entry) {
    if (is_new) *is_new = false;

    // Move to front
    if (entry != lru->head) {
      lru_unlink(lru, entry);
      lru_push_front(lru, entry);
    }
    return lru_entry_value(entry);
  }

  // Need a new slot
  if (is_new) *is_new = true;

  entry = lru_find_free(lru);
  if (!entry) {
    // Cache full, evict LRU (tail)
    entry = lru->tail;
    if (!entry) return NULL;

    lru_unlink(lru, entry);
    lru->count--;
  }

  // Set up new entry
  entry->key = key;
  entry->occupied = true;
  lru_push_front(lru, entry);
  lru->count++;

  return lru_entry_value(entry);
}

bool lru_remove(lru_t *lru, uint32_t key) {
  if (!lru) return false;

  lru_entry_t *entry = lru_find(lru, key);
  if (!entry) return false;

  lru_unlink(lru, entry);
  entry->key = 0;
  entry->occupied = false;
  lru->count--;

  return true;
}

void lru_clear(lru_t *lru) {
  if (!lru) return;

  for (uint32_t i = 0; i < lru->max_entries; i++) {
    lru_entry_t *entry = lru_entry_at(lru, i);
    if (entry->occupied) {
      entry->key = 0;
      entry->occupied = false;
      entry->prev = NULL;
      entry->next = NULL;
    }
  }

  lru->head = NULL;
  lru->tail = NULL;
  lru->count = 0;
}

uint32_t lru_count(lru_t *lru) {
  return lru ? lru->count : 0;
}

uint32_t lru_elem_size(lru_t *lru) {
  return lru ? lru->elem_size : 0;
}
