#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../src/lru.h"

// ============== Test Framework ==============
int tests_run = 0;
int tests_passed = 0;

#define TEST(name) void name(void); \
  void name##_runner(void) { \
    printf("Running %s... ", #name); \
    tests_run++; \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
  } \
  void name(void)

#define RUN_TEST(name) name##_runner()

#define ASSERT(cond) do { \
  if (!(cond)) { \
    printf("FAIL\n  Assertion failed: %s\n  at %s:%d\n", #cond, __FILE__, __LINE__); \
    exit(1); \
  } \
} while(0)

#define ASSERT_EQ(a, b) do { \
  if ((a) != (b)) { \
    printf("FAIL\n  Expected %ld == %ld\n  at %s:%d\n", (long)(a), (long)(b), __FILE__, __LINE__); \
    exit(1); \
  } \
} while(0)

#define ASSERT_NULL(ptr) ASSERT((ptr) == NULL)
#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != NULL)

// ============== Tests ==============

TEST(test_init_free) {
  lru_t *lru = lru_init(10, sizeof(int));
  ASSERT_NOT_NULL(lru);
  ASSERT_EQ(lru_count(lru), 0);
  ASSERT_EQ(lru_elem_size(lru), sizeof(int));
  lru_free(lru);
}

TEST(test_init_zero_entries) {
  lru_t *lru = lru_init(0, sizeof(int));
  ASSERT_NULL(lru);
}

TEST(test_init_zero_elem_size) {
  lru_t *lru = lru_init(10, 0);
  ASSERT_NULL(lru);
}

TEST(test_set_get_single) {
  lru_t *lru = lru_init(10, sizeof(int));

  int value = 42;
  void *stored = lru_set(lru, 100, &value);
  ASSERT_NOT_NULL(stored);
  ASSERT_EQ(lru_count(lru), 1);

  int *result = (int *)lru_get(lru, 100);
  ASSERT_NOT_NULL(result);
  ASSERT_EQ(*result, 42);

  lru_free(lru);
}

TEST(test_get_nonexistent) {
  lru_t *lru = lru_init(10, sizeof(int));

  void *result = lru_get(lru, 999);
  ASSERT_NULL(result);

  lru_free(lru);
}

TEST(test_set_get_multiple) {
  lru_t *lru = lru_init(10, sizeof(int));

  int values[5] = {10, 20, 30, 40, 50};
  for (int i = 0; i < 5; i++) {
    ASSERT_NOT_NULL(lru_set(lru, i, &values[i]));
  }
  ASSERT_EQ(lru_count(lru), 5);

  for (int i = 0; i < 5; i++) {
    int *result = (int *)lru_get(lru, i);
    ASSERT_NOT_NULL(result);
    ASSERT_EQ(*result, (i + 1) * 10);
  }

  lru_free(lru);
}

TEST(test_update_existing) {
  lru_t *lru = lru_init(10, sizeof(int));

  int value1 = 100;
  int value2 = 200;

  lru_set(lru, 1, &value1);
  ASSERT_EQ(lru_count(lru), 1);

  lru_set(lru, 1, &value2);
  ASSERT_EQ(lru_count(lru), 1);  // Count should stay the same

  int *result = (int *)lru_get(lru, 1);
  ASSERT_EQ(*result, 200);

  lru_free(lru);
}

TEST(test_eviction_lru) {
  lru_t *lru = lru_init(3, sizeof(int));

  int v1 = 1, v2 = 2, v3 = 3, v4 = 4;

  // Fill cache
  lru_set(lru, 1, &v1);
  lru_set(lru, 2, &v2);
  lru_set(lru, 3, &v3);
  ASSERT_EQ(lru_count(lru), 3);

  // Access key 1 to make it recently used
  lru_get(lru, 1);

  // Add key 4, should evict key 2 (LRU)
  lru_set(lru, 4, &v4);
  ASSERT_EQ(lru_count(lru), 3);

  // Key 2 should be evicted
  ASSERT_NULL(lru_get(lru, 2));

  // Keys 1, 3, 4 should still exist
  ASSERT_NOT_NULL(lru_get(lru, 1));
  ASSERT_NOT_NULL(lru_get(lru, 3));
  ASSERT_NOT_NULL(lru_get(lru, 4));

  lru_free(lru);
}

TEST(test_eviction_order) {
  lru_t *lru = lru_init(3, sizeof(int));

  int values[6];
  for (int i = 0; i < 6; i++) values[i] = i * 10;

  // Add 1, 2, 3
  lru_set(lru, 1, &values[1]);
  lru_set(lru, 2, &values[2]);
  lru_set(lru, 3, &values[3]);

  // Add 4, evicts 1
  lru_set(lru, 4, &values[4]);
  ASSERT_NULL(lru_get(lru, 1));

  // Add 5, evicts 2
  lru_set(lru, 5, &values[5]);
  ASSERT_NULL(lru_get(lru, 2));

  // Remaining: 3, 4, 5
  ASSERT_NOT_NULL(lru_get(lru, 3));
  ASSERT_NOT_NULL(lru_get(lru, 4));
  ASSERT_NOT_NULL(lru_get(lru, 5));

  lru_free(lru);
}

TEST(test_remove) {
  lru_t *lru = lru_init(10, sizeof(int));

  int v1 = 1, v2 = 2, v3 = 3;
  lru_set(lru, 1, &v1);
  lru_set(lru, 2, &v2);
  lru_set(lru, 3, &v3);
  ASSERT_EQ(lru_count(lru), 3);

  // Remove middle entry
  bool removed = lru_remove(lru, 2);
  ASSERT(removed);
  ASSERT_EQ(lru_count(lru), 2);

  // Should not find removed entry
  ASSERT_NULL(lru_get(lru, 2));

  // Other entries should still exist
  ASSERT_NOT_NULL(lru_get(lru, 1));
  ASSERT_NOT_NULL(lru_get(lru, 3));

  lru_free(lru);
}

TEST(test_remove_nonexistent) {
  lru_t *lru = lru_init(10, sizeof(int));

  bool removed = lru_remove(lru, 999);
  ASSERT(!removed);

  lru_free(lru);
}

TEST(test_clear) {
  lru_t *lru = lru_init(10, sizeof(int));

  int v1 = 1, v2 = 2;
  lru_set(lru, 1, &v1);
  lru_set(lru, 2, &v2);
  ASSERT_EQ(lru_count(lru), 2);

  lru_clear(lru);
  ASSERT_EQ(lru_count(lru), 0);

  // Should not find anything
  ASSERT_NULL(lru_get(lru, 1));
  ASSERT_NULL(lru_get(lru, 2));

  // Can add new entries after clear
  int v3 = 3;
  lru_set(lru, 3, &v3);
  ASSERT_EQ(lru_count(lru), 1);

  lru_free(lru);
}

TEST(test_large_keys) {
  lru_t *lru = lru_init(10, sizeof(int));

  int v1 = 1, v2 = 2;
  uint32_t key1 = 0xFFFFFFFF;
  uint32_t key2 = 0x12345678;

  lru_set(lru, key1, &v1);
  lru_set(lru, key2, &v2);

  ASSERT_EQ(*(int *)lru_get(lru, key1), 1);
  ASSERT_EQ(*(int *)lru_get(lru, key2), 2);

  lru_free(lru);
}

TEST(test_sector_cache_simulation) {
  // Simulate sector cache usage pattern with 512-byte sectors
  lru_t *lru = lru_init(36, 512);  // 2 tracks worth

  // Simulate reading track 0, side 0 (sectors 1-18)
  uint8_t sector_data[512];
  for (int s = 0; s < 18; s++) {
    memset(sector_data, s, 512);  // Fill with sector number
    uint32_t key = (0 << 16) | (0 << 8) | (s + 1);  // track=0, side=0, sector=s+1
    lru_set(lru, key, sector_data);
  }
  ASSERT_EQ(lru_count(lru), 18);

  // Read track 0, side 1
  for (int s = 0; s < 18; s++) {
    memset(sector_data, 100 + s, 512);
    uint32_t key = (0 << 16) | (1 << 8) | (s + 1);
    lru_set(lru, key, sector_data);
  }
  ASSERT_EQ(lru_count(lru), 36);

  // All sectors should be cached
  for (int side = 0; side < 2; side++) {
    for (int s = 0; s < 18; s++) {
      uint32_t key = (0 << 16) | (side << 8) | (s + 1);
      uint8_t *val = (uint8_t *)lru_get(lru, key);
      ASSERT_NOT_NULL(val);
      ASSERT_EQ(val[0], (uint8_t)(side * 100 + s));
    }
  }

  // Read track 1 - should evict track 0 side 0
  for (int s = 0; s < 18; s++) {
    memset(sector_data, 200 + s, 512);
    uint32_t key = (1 << 16) | (0 << 8) | (s + 1);
    lru_set(lru, key, sector_data);
  }

  lru_free(lru);
}

TEST(test_null_lru) {
  // All functions should handle NULL gracefully
  ASSERT_NULL(lru_get(NULL, 0));
  ASSERT_NULL(lru_set(NULL, 0, NULL));
  ASSERT(!lru_remove(NULL, 0));
  ASSERT_EQ(lru_count(NULL), 0);
  ASSERT_EQ(lru_elem_size(NULL), 0);
  lru_clear(NULL);  // Should not crash
  lru_free(NULL);   // Should not crash
}

TEST(test_single_entry_cache) {
  lru_t *lru = lru_init(1, sizeof(int));

  int v1 = 1, v2 = 2, v3 = 3;

  lru_set(lru, 1, &v1);
  ASSERT_EQ(*(int *)lru_get(lru, 1), 1);

  lru_set(lru, 2, &v2);
  ASSERT_NULL(lru_get(lru, 1));
  ASSERT_EQ(*(int *)lru_get(lru, 2), 2);

  lru_set(lru, 3, &v3);
  ASSERT_NULL(lru_get(lru, 2));
  ASSERT_EQ(*(int *)lru_get(lru, 3), 3);

  lru_free(lru);
}

TEST(test_get_or_create) {
  lru_t *lru = lru_init(10, sizeof(int));
  bool is_new;

  // First access should be new
  int *slot = (int *)lru_get_or_create(lru, 1, &is_new);
  ASSERT_NOT_NULL(slot);
  ASSERT(is_new);
  *slot = 42;
  ASSERT_EQ(lru_count(lru), 1);

  // Second access should not be new
  slot = (int *)lru_get_or_create(lru, 1, &is_new);
  ASSERT_NOT_NULL(slot);
  ASSERT(!is_new);
  ASSERT_EQ(*slot, 42);
  ASSERT_EQ(lru_count(lru), 1);

  // Different key should be new
  slot = (int *)lru_get_or_create(lru, 2, &is_new);
  ASSERT_NOT_NULL(slot);
  ASSERT(is_new);
  ASSERT_EQ(lru_count(lru), 2);

  lru_free(lru);
}

TEST(test_get_or_create_eviction) {
  lru_t *lru = lru_init(2, sizeof(int));
  bool is_new;

  // Fill cache
  int *s1 = (int *)lru_get_or_create(lru, 1, &is_new);
  *s1 = 100;
  ASSERT(is_new);

  int *s2 = (int *)lru_get_or_create(lru, 2, &is_new);
  *s2 = 200;
  ASSERT(is_new);

  // Add third - should evict key 1
  int *s3 = (int *)lru_get_or_create(lru, 3, &is_new);
  *s3 = 300;
  ASSERT(is_new);
  ASSERT_EQ(lru_count(lru), 2);

  // Key 1 should be evicted
  ASSERT_NULL(lru_get(lru, 1));

  // Keys 2, 3 should exist
  ASSERT_EQ(*(int *)lru_get(lru, 2), 200);
  ASSERT_EQ(*(int *)lru_get(lru, 3), 300);

  lru_free(lru);
}

TEST(test_set_null_value) {
  // Setting with NULL value should zero the storage
  lru_t *lru = lru_init(10, sizeof(int));

  void *slot = lru_set(lru, 1, NULL);
  ASSERT_NOT_NULL(slot);
  ASSERT_EQ(*(int *)slot, 0);

  lru_free(lru);
}

TEST(test_large_elem_size) {
  // Test with larger element size (like a sector)
  typedef struct {
    uint8_t data[512];
    uint32_t checksum;
  } sector_t;

  lru_t *lru = lru_init(5, sizeof(sector_t));
  ASSERT_EQ(lru_elem_size(lru), sizeof(sector_t));

  sector_t src;
  memset(src.data, 0xAB, 512);
  src.checksum = 0x12345678;

  sector_t *stored = (sector_t *)lru_set(lru, 100, &src);
  ASSERT_NOT_NULL(stored);
  ASSERT_EQ(stored->data[0], 0xAB);
  ASSERT_EQ(stored->data[511], 0xAB);
  ASSERT_EQ(stored->checksum, 0x12345678);

  sector_t *retrieved = (sector_t *)lru_get(lru, 100);
  ASSERT_NOT_NULL(retrieved);
  ASSERT_EQ(retrieved->data[0], 0xAB);
  ASSERT_EQ(retrieved->checksum, 0x12345678);

  lru_free(lru);
}

TEST(test_direct_write_to_slot) {
  // Test writing directly to a slot obtained from get_or_create
  lru_t *lru = lru_init(10, 512);
  bool is_new;

  // Get a slot and write directly to it
  uint8_t *sector = (uint8_t *)lru_get_or_create(lru, 1, &is_new);
  ASSERT(is_new);

  // Write data directly
  for (int i = 0; i < 512; i++) {
    sector[i] = i & 0xFF;
  }

  // Retrieve and verify
  uint8_t *retrieved = (uint8_t *)lru_get(lru, 1);
  ASSERT_NOT_NULL(retrieved);
  for (int i = 0; i < 512; i++) {
    ASSERT_EQ(retrieved[i], i & 0xFF);
  }

  lru_free(lru);
}

// ============== Main ==============

int main(void) {
  printf("=== LRU Cache Tests ===\n\n");

  RUN_TEST(test_init_free);
  RUN_TEST(test_init_zero_entries);
  RUN_TEST(test_init_zero_elem_size);
  RUN_TEST(test_set_get_single);
  RUN_TEST(test_get_nonexistent);
  RUN_TEST(test_set_get_multiple);
  RUN_TEST(test_update_existing);
  RUN_TEST(test_eviction_lru);
  RUN_TEST(test_eviction_order);
  RUN_TEST(test_remove);
  RUN_TEST(test_remove_nonexistent);
  RUN_TEST(test_clear);
  RUN_TEST(test_large_keys);
  RUN_TEST(test_sector_cache_simulation);
  RUN_TEST(test_null_lru);
  RUN_TEST(test_single_entry_cache);
  RUN_TEST(test_get_or_create);
  RUN_TEST(test_get_or_create_eviction);
  RUN_TEST(test_set_null_value);
  RUN_TEST(test_large_elem_size);
  RUN_TEST(test_direct_write_to_slot);

  printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);

  return (tests_passed == tests_run) ? 0 : 1;
}
