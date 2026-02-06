#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

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
  long _a = (long)(a), _b = (long)(b); \
  if (_a != _b) { \
    printf("FAIL\n  Expected %ld == %ld\n  at %s:%d\n", _a, _b, __FILE__, __LINE__); \
    exit(1); \
  } \
} while(0)

#define ASSERT_MEM_EQ(a, b, len) do { \
  if (memcmp(a, b, len) != 0) { \
    printf("FAIL\n  Memory mismatch at %s:%d\n", __FILE__, __LINE__); \
    exit(1); \
  } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
  if (strcmp(a, b) != 0) { \
    printf("FAIL\n  Expected \"%s\" == \"%s\"\n  at %s:%d\n", a, b, __FILE__, __LINE__); \
    exit(1); \
  } \
} while(0)

#define ASSERT_NULL(ptr) ASSERT((ptr) == NULL)
#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != NULL)

#define TEST_RESULTS() do { \
  printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run); \
  return (tests_passed == tests_run) ? 0 : 1; \
} while(0)

#endif
