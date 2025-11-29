#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../src/floppy.h"
#include "../src/f12.h"

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
    printf("FAIL\n  Expected %d == %d\n  at %s:%d\n", (int)(a), (int)(b), __FILE__, __LINE__); \
    exit(1); \
  } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
  if (strcmp(a, b) != 0) { \
    printf("FAIL\n  Expected \"%s\" == \"%s\"\n  at %s:%d\n", a, b, __FILE__, __LINE__); \
    exit(1); \
  } \
} while(0)

// ============== Virtual Disk ==============
#define VDISK_TOTAL_SECTORS (80 * 2 * 18)

typedef struct {
  uint8_t data[VDISK_TOTAL_SECTORS][SECTOR_SIZE];
  bool write_protected;
  bool disk_changed;
} vdisk_t;

static vdisk_t vdisk;

static inline int vdisk_lba(uint8_t track, uint8_t side, uint8_t sector_n) {
  return track * 36 + side * 18 + (sector_n - 1);
}

static bool vdisk_read(void *ctx, sector_t *sector) {
  (void)ctx;
  int lba = vdisk_lba(sector->track, sector->side, sector->sector_n);
  if (lba < 0 || lba >= VDISK_TOTAL_SECTORS) {
    sector->valid = false;
    return false;
  }
  memcpy(sector->data, vdisk.data[lba], SECTOR_SIZE);
  sector->valid = true;
  return true;
}

static bool vdisk_write(void *ctx, track_t *track) {
  (void)ctx;
  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    sector_t *s = &track->sectors[i];
    if (s->valid) {
      int lba = vdisk_lba(track->track, track->side, s->sector_n);
      if (lba >= 0 && lba < VDISK_TOTAL_SECTORS) {
        memcpy(vdisk.data[lba], s->data, SECTOR_SIZE);
      }
    }
  }
  return true;
}

static bool vdisk_disk_changed(void *ctx) {
  (void)ctx;
  if (vdisk.disk_changed) {
    vdisk.disk_changed = false;  // Clear on read
    return true;
  }
  return false;
}

static bool vdisk_write_protected(void *ctx) {
  (void)ctx;
  return vdisk.write_protected;
}

static void vdisk_init(void) {
  memset(&vdisk, 0, sizeof(vdisk));
  vdisk.write_protected = false;
  vdisk.disk_changed = false;
}

static f12_io_t vdisk_io = {
  .read = vdisk_read,
  .write = vdisk_write,
  .disk_changed = vdisk_disk_changed,
  .write_protected = vdisk_write_protected,
  .ctx = NULL,
};

// ============== Tests ==============

TEST(test_mount_unmount) {
  vdisk_init();

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_io;

  // Format works without mount first (just needs io)
  f12_err_t err = f12_format(&fs, "TEST", false);
  ASSERT_EQ(err, F12_OK);

  // Now mount
  err = f12_mount(&fs, vdisk_io);
  ASSERT_EQ(err, F12_OK);
  ASSERT(fs.mounted);

  f12_unmount(&fs);
  ASSERT(!fs.mounted);
}

TEST(test_format_and_mount) {
  vdisk_init();

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_io;

  // Format
  f12_err_t err = f12_format(&fs, "TESTDISK", false);
  ASSERT_EQ(err, F12_OK);

  // Mount
  err = f12_mount(&fs, vdisk_io);
  ASSERT_EQ(err, F12_OK);

  // Clean up
  f12_unmount(&fs);
}

TEST(test_create_write_read_file) {
  vdisk_init();

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_io;
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_io);

  // Write file
  f12_file_t *f = f12_open(&fs, "HELLO.TXT", "w");
  ASSERT(f != NULL);

  const char *msg = "Hello, World!";
  int n = f12_write(f, msg, strlen(msg));
  ASSERT_EQ(n, (int)strlen(msg));

  f12_err_t err = f12_close(f);
  ASSERT_EQ(err, F12_OK);

  // Read it back
  f = f12_open(&fs, "HELLO.TXT", "r");
  ASSERT(f != NULL);

  char buf[64];
  n = f12_read(f, buf, sizeof(buf));
  ASSERT_EQ(n, (int)strlen(msg));
  buf[n] = '\0';
  ASSERT_STR_EQ(buf, msg);

  f12_close(f);
  f12_unmount(&fs);
}

TEST(test_file_stat) {
  vdisk_init();

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_io;
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_io);

  // Create file
  f12_file_t *f = f12_open(&fs, "DATA.BIN", "w");
  ASSERT(f != NULL);

  uint8_t data[256];
  for (int i = 0; i < 256; i++) data[i] = i;
  f12_write(f, data, sizeof(data));
  f12_close(f);

  // Stat the file
  f12_stat_t stat;
  f12_err_t err = f12_stat(&fs, "DATA.BIN", &stat);
  ASSERT_EQ(err, F12_OK);
  ASSERT_STR_EQ(stat.name, "DATA.BIN");
  ASSERT_EQ(stat.size, 256);
  ASSERT(!stat.is_dir);

  f12_unmount(&fs);
}

TEST(test_file_delete) {
  vdisk_init();

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_io;
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_io);

  // Create file
  f12_file_t *f = f12_open(&fs, "TODEL.TXT", "w");
  ASSERT(f != NULL);
  f12_write(f, "delete me", 9);
  f12_close(f);

  // Verify exists
  f12_stat_t stat;
  f12_err_t err = f12_stat(&fs, "TODEL.TXT", &stat);
  ASSERT_EQ(err, F12_OK);

  // Delete
  err = f12_delete(&fs, "TODEL.TXT");
  ASSERT_EQ(err, F12_OK);

  // Verify gone
  err = f12_stat(&fs, "TODEL.TXT", &stat);
  ASSERT_EQ(err, F12_ERR_NOT_FOUND);

  f12_unmount(&fs);
}

TEST(test_directory_listing) {
  vdisk_init();

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_io;
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_io);

  // Create some files
  f12_file_t *f;

  f = f12_open(&fs, "FILE1.TXT", "w");
  f12_write(f, "one", 3);
  f12_close(f);

  f = f12_open(&fs, "FILE2.TXT", "w");
  f12_write(f, "two", 3);
  f12_close(f);

  f = f12_open(&fs, "FILE3.TXT", "w");
  f12_write(f, "three", 5);
  f12_close(f);

  // List directory
  f12_dir_t dir;
  f12_err_t err = f12_opendir(&fs, "/", &dir);
  ASSERT_EQ(err, F12_OK);

  int count = 0;
  f12_stat_t stat;
  while (f12_readdir(&dir, &stat) == F12_OK) {
    count++;
    ASSERT(strlen(stat.name) > 0);
  }

  f12_closedir(&dir);
  ASSERT_EQ(count, 3);

  f12_unmount(&fs);
}

TEST(test_too_many_open_files) {
  vdisk_init();

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_io;
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_io);

  // Create files
  for (int i = 0; i < F12_MAX_OPEN_FILES + 2; i++) {
    char name[16];
    snprintf(name, sizeof(name), "FILE%d.TXT", i);
    f12_file_t *f = f12_open(&fs, name, "w");
    if (f) {
      f12_write(f, "x", 1);
      f12_close(f);
    }
  }

  // Open max files
  f12_file_t *files[F12_MAX_OPEN_FILES];
  for (int i = 0; i < F12_MAX_OPEN_FILES; i++) {
    char name[16];
    snprintf(name, sizeof(name), "FILE%d.TXT", i);
    files[i] = f12_open(&fs, name, "r");
    ASSERT(files[i] != NULL);
  }

  // Try to open one more
  f12_file_t *extra = f12_open(&fs, "FILE10.TXT", "r");
  ASSERT(extra == NULL);
  ASSERT_EQ(f12_errno(&fs), F12_ERR_TOO_MANY);

  // Close all
  for (int i = 0; i < F12_MAX_OPEN_FILES; i++) {
    f12_close(files[i]);
  }

  f12_unmount(&fs);
}

TEST(test_write_protected) {
  vdisk_init();

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_io;
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_io);

  // Create a file first
  f12_file_t *f = f12_open(&fs, "TEST.TXT", "w");
  ASSERT(f != NULL);
  f12_write(f, "test", 4);
  f12_close(f);

  // Enable write protection
  vdisk.write_protected = true;

  // Try to write
  f = f12_open(&fs, "TEST2.TXT", "w");
  ASSERT(f == NULL);
  ASSERT_EQ(f12_errno(&fs), F12_ERR_WRITE_PROTECTED);

  // Try to delete
  f12_err_t err = f12_delete(&fs, "TEST.TXT");
  ASSERT_EQ(err, F12_ERR_WRITE_PROTECTED);

  // Reading should still work
  f = f12_open(&fs, "TEST.TXT", "r");
  ASSERT(f != NULL);
  char buf[16];
  int n = f12_read(f, buf, sizeof(buf));
  ASSERT_EQ(n, 4);
  f12_close(f);

  f12_unmount(&fs);
}

TEST(test_disk_changed) {
  vdisk_init();

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_io;
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_io);

  // Create a file
  f12_file_t *f = f12_open(&fs, "TEST.TXT", "w");
  ASSERT(f != NULL);
  f12_write(f, "hello", 5);
  f12_close(f);

  // Simulate disk change
  vdisk.disk_changed = true;

  // Operations should fail
  f = f12_open(&fs, "TEST.TXT", "r");
  ASSERT(f == NULL);
  ASSERT_EQ(f12_errno(&fs), F12_ERR_DISK_CHANGED);

  // Must remount
  ASSERT(!fs.mounted);

  // Remount (format fresh disk for this test)
  vdisk_init();
  f12_format(&fs, "NEW", false);
  f12_err_t err = f12_mount(&fs, vdisk_io);
  ASSERT_EQ(err, F12_OK);

  f12_unmount(&fs);
}

TEST(test_seek_and_tell) {
  vdisk_init();

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_io;
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_io);

  // Write file with known content
  f12_file_t *f = f12_open(&fs, "SEEK.TXT", "w");
  ASSERT(f != NULL);
  f12_write(f, "0123456789ABCDEF", 16);
  f12_close(f);

  // Read with seek
  f = f12_open(&fs, "SEEK.TXT", "r");
  ASSERT(f != NULL);

  ASSERT_EQ(f12_tell(f), 0);

  // Seek to middle
  f12_err_t err = f12_seek(f, 8);
  ASSERT_EQ(err, F12_OK);
  ASSERT_EQ(f12_tell(f), 8);

  char buf[8];
  int n = f12_read(f, buf, 4);
  ASSERT_EQ(n, 4);
  buf[4] = '\0';
  ASSERT_STR_EQ(buf, "89AB");

  f12_close(f);
  f12_unmount(&fs);
}

TEST(test_read_at) {
  vdisk_init();

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_io;
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_io);

  // Write file
  f12_file_t *f = f12_open(&fs, "RAND.TXT", "w");
  f12_write(f, "AAAABBBBCCCCDDDD", 16);
  f12_close(f);

  // Read at various positions
  f = f12_open(&fs, "RAND.TXT", "r");
  ASSERT(f != NULL);

  char buf[5];

  // Read from offset 4
  int n = f12_read_at(f, 4, buf, 4);
  ASSERT_EQ(n, 4);
  buf[4] = '\0';
  ASSERT_STR_EQ(buf, "BBBB");

  // Read from offset 12
  n = f12_read_at(f, 12, buf, 4);
  ASSERT_EQ(n, 4);
  buf[4] = '\0';
  ASSERT_STR_EQ(buf, "DDDD");

  f12_close(f);
  f12_unmount(&fs);
}

TEST(test_file_not_found) {
  vdisk_init();

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_io;
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_io);

  // Try to open nonexistent file
  f12_file_t *f = f12_open(&fs, "NOTHERE.TXT", "r");
  ASSERT(f == NULL);
  ASSERT_EQ(f12_errno(&fs), F12_ERR_NOT_FOUND);

  // Stat nonexistent
  f12_stat_t stat;
  f12_err_t err = f12_stat(&fs, "NOTHERE.TXT", &stat);
  ASSERT_EQ(err, F12_ERR_NOT_FOUND);

  f12_unmount(&fs);
}

TEST(test_large_file) {
  vdisk_init();

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_io;
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_io);

  // Write large file (multiple clusters)
  f12_file_t *f = f12_open(&fs, "LARGE.BIN", "w");
  ASSERT(f != NULL);

  uint8_t block[512];
  uint32_t total = 0;
  for (int i = 0; i < 20; i++) {  // 10KB
    memset(block, i, sizeof(block));
    int n = f12_write(f, block, sizeof(block));
    ASSERT(n > 0);
    total += n;
  }
  f12_close(f);

  // Verify size
  f12_stat_t stat;
  f12_err_t err = f12_stat(&fs, "LARGE.BIN", &stat);
  ASSERT_EQ(err, F12_OK);
  ASSERT_EQ(stat.size, total);

  // Read back and verify
  f = f12_open(&fs, "LARGE.BIN", "r");
  ASSERT(f != NULL);

  for (int i = 0; i < 20; i++) {
    uint8_t expected[512];
    memset(expected, i, sizeof(expected));
    int n = f12_read(f, block, sizeof(block));
    ASSERT_EQ(n, 512);
    ASSERT(memcmp(block, expected, sizeof(block)) == 0);
  }

  f12_close(f);
  f12_unmount(&fs);
}

TEST(test_strerror) {
  ASSERT_STR_EQ(f12_strerror(F12_OK), "Success");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_NOT_FOUND), "File not found");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_DISK_CHANGED), "Disk changed");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_WRITE_PROTECTED), "Write protected");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_TOO_MANY), "Too many open files");
}

// Simple callback for test_list_callback_proper
static void list_counter(const f12_stat_t *stat, void *ctx) {
  (void)stat;
  int *count = (int *)ctx;
  (*count)++;
}

TEST(test_list_callback_proper) {
  vdisk_init();

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_io;
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_io);

  // Create files
  for (int i = 0; i < 5; i++) {
    char name[16];
    snprintf(name, sizeof(name), "F%d.TXT", i);
    f12_file_t *f = f12_open(&fs, name, "w");
    f12_write(f, "x", 1);
    f12_close(f);
  }

  // Count via callback
  int count = 0;
  f12_err_t err = f12_list(&fs, list_counter, &count);
  ASSERT_EQ(err, F12_OK);
  ASSERT_EQ(count, 5);

  f12_unmount(&fs);
}

// ============== Main ==============

int main(void) {
  printf("=== F12 High-Level API Tests ===\n\n");

  RUN_TEST(test_mount_unmount);
  RUN_TEST(test_format_and_mount);
  RUN_TEST(test_create_write_read_file);
  RUN_TEST(test_file_stat);
  RUN_TEST(test_file_delete);
  RUN_TEST(test_directory_listing);
  RUN_TEST(test_too_many_open_files);
  RUN_TEST(test_write_protected);
  RUN_TEST(test_disk_changed);
  RUN_TEST(test_seek_and_tell);
  RUN_TEST(test_read_at);
  RUN_TEST(test_file_not_found);
  RUN_TEST(test_large_file);
  RUN_TEST(test_strerror);
  RUN_TEST(test_list_callback_proper);

  printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
  return (tests_passed == tests_run) ? 0 : 1;
}
