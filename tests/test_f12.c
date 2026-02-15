#include "test.h"
#include "vdisk.h"
#include "../src/f12.h"

static vdisk_t vdisk;

static f12_io_t vdisk_f12_io(void) {
  return (f12_io_t){
    .read = vdisk_read,
    .write = vdisk_write,
    .disk_changed = vdisk_disk_changed,
    .write_protected = vdisk_write_protected,
    .ctx = &vdisk,
  };
}

TEST(test_mount_unmount) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();

  f12_err_t err = f12_format(&fs, "TEST", false);
  ASSERT_EQ(err, F12_OK);

  err = f12_mount(&fs, vdisk_f12_io());
  ASSERT_EQ(err, F12_OK);
  ASSERT(fs.mounted);

  f12_unmount(&fs);
  ASSERT(!fs.mounted);
}

TEST(test_format_and_mount) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();

  f12_err_t err = f12_format(&fs, "TESTDISK", false);
  ASSERT_EQ(err, F12_OK);

  err = f12_mount(&fs, vdisk_f12_io());
  ASSERT_EQ(err, F12_OK);

  f12_unmount(&fs);
}

TEST(test_create_write_read_file) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "HELLO.TXT", "w");
  ASSERT(f != NULL);

  const char *msg = "Hello, World!";
  int n = f12_write(f, msg, strlen(msg));
  ASSERT_EQ(n, (int)strlen(msg));

  f12_err_t err = f12_close(f);
  ASSERT_EQ(err, F12_OK);

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
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "DATA.BIN", "w");
  ASSERT(f != NULL);

  uint8_t data[256];
  for (int i = 0; i < 256; i++) data[i] = i;
  f12_write(f, data, sizeof(data));
  f12_close(f);

  f12_stat_t stat;
  f12_err_t err = f12_stat(&fs, "DATA.BIN", &stat);
  ASSERT_EQ(err, F12_OK);
  ASSERT_STR_EQ(stat.name, "DATA.BIN");
  ASSERT_EQ(stat.size, 256);
  ASSERT(!stat.is_dir);

  f12_unmount(&fs);
}

TEST(test_file_delete) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "TODEL.TXT", "w");
  ASSERT(f != NULL);
  f12_write(f, "delete me", 9);
  f12_close(f);

  f12_stat_t stat;
  f12_err_t err = f12_stat(&fs, "TODEL.TXT", &stat);
  ASSERT_EQ(err, F12_OK);

  err = f12_delete(&fs, "TODEL.TXT");
  ASSERT_EQ(err, F12_OK);

  err = f12_stat(&fs, "TODEL.TXT", &stat);
  ASSERT_EQ(err, F12_ERR_NOT_FOUND);

  f12_unmount(&fs);
}

TEST(test_directory_listing) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

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
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  for (int i = 0; i < F12_MAX_OPEN_FILES + 2; i++) {
    char name[16];
    snprintf(name, sizeof(name), "FILE%d.TXT", i);
    f12_file_t *f = f12_open(&fs, name, "w");
    if (f) {
      f12_write(f, "x", 1);
      f12_close(f);
    }
  }

  f12_file_t *files[F12_MAX_OPEN_FILES];
  for (int i = 0; i < F12_MAX_OPEN_FILES; i++) {
    char name[16];
    snprintf(name, sizeof(name), "FILE%d.TXT", i);
    files[i] = f12_open(&fs, name, "r");
    ASSERT(files[i] != NULL);
  }

  f12_file_t *extra = f12_open(&fs, "FILE10.TXT", "r");
  ASSERT(extra == NULL);
  ASSERT_EQ(f12_errno(&fs), F12_ERR_TOO_MANY);

  for (int i = 0; i < F12_MAX_OPEN_FILES; i++) {
    f12_close(files[i]);
  }

  f12_unmount(&fs);
}

TEST(test_write_protected) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "TEST.TXT", "w");
  ASSERT(f != NULL);
  f12_write(f, "test", 4);
  f12_close(f);

  vdisk.write_protected = true;

  f = f12_open(&fs, "TEST2.TXT", "w");
  ASSERT(f == NULL);
  ASSERT_EQ(f12_errno(&fs), F12_ERR_WRITE_PROTECTED);

  f12_err_t err = f12_delete(&fs, "TEST.TXT");
  ASSERT_EQ(err, F12_ERR_WRITE_PROTECTED);

  f = f12_open(&fs, "TEST.TXT", "r");
  ASSERT(f != NULL);
  char buf[16];
  int n = f12_read(f, buf, sizeof(buf));
  ASSERT_EQ(n, 4);
  f12_close(f);

  f12_unmount(&fs);
}

TEST(test_disk_changed) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "TEST.TXT", "w");
  ASSERT(f != NULL);
  f12_write(f, "hello", 5);
  f12_close(f);

  vdisk.disk_changed = true;

  f = f12_open(&fs, "TEST.TXT", "r");
  ASSERT(f == NULL);
  ASSERT_EQ(f12_errno(&fs), F12_ERR_DISK_CHANGED);

  ASSERT(!fs.mounted);

  vdisk_init(&vdisk);
  fs.io = vdisk_f12_io();
  f12_format(&fs, "NEW", false);
  f12_err_t err = f12_mount(&fs, vdisk_f12_io());
  ASSERT_EQ(err, F12_OK);

  f12_unmount(&fs);
}

TEST(test_seek_and_tell) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "SEEK.TXT", "w");
  ASSERT(f != NULL);
  f12_write(f, "0123456789ABCDEF", 16);
  f12_close(f);

  f = f12_open(&fs, "SEEK.TXT", "r");
  ASSERT(f != NULL);

  ASSERT_EQ(f12_tell(f), 0);

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
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "RAND.TXT", "w");
  f12_write(f, "AAAABBBBCCCCDDDD", 16);
  f12_close(f);

  f = f12_open(&fs, "RAND.TXT", "r");
  ASSERT(f != NULL);

  char buf[5];

  int n = f12_read_at(f, 4, buf, 4);
  ASSERT_EQ(n, 4);
  buf[4] = '\0';
  ASSERT_STR_EQ(buf, "BBBB");

  n = f12_read_at(f, 12, buf, 4);
  ASSERT_EQ(n, 4);
  buf[4] = '\0';
  ASSERT_STR_EQ(buf, "DDDD");

  f12_close(f);
  f12_unmount(&fs);
}

TEST(test_file_not_found) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "NOTHERE.TXT", "r");
  ASSERT(f == NULL);
  ASSERT_EQ(f12_errno(&fs), F12_ERR_NOT_FOUND);

  f12_stat_t stat;
  f12_err_t err = f12_stat(&fs, "NOTHERE.TXT", &stat);
  ASSERT_EQ(err, F12_ERR_NOT_FOUND);

  f12_unmount(&fs);
}

TEST(test_large_file) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "LARGE.BIN", "w");
  ASSERT(f != NULL);

  uint8_t block[512];
  uint32_t total = 0;
  for (int i = 0; i < 20; i++) {
    memset(block, i, sizeof(block));
    int n = f12_write(f, block, sizeof(block));
    ASSERT(n > 0);
    total += n;
  }
  f12_close(f);

  f12_stat_t stat;
  f12_err_t err = f12_stat(&fs, "LARGE.BIN", &stat);
  ASSERT_EQ(err, F12_OK);
  ASSERT_EQ(stat.size, total);

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

TEST(test_multiple_small_writes) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "MULTI.TXT", "w");
  ASSERT(f != NULL);

  const char *lines[] = { "a\n", "b\n", "c\n", "d\n", "e\n", "f\n" };
  for (int i = 0; i < 6; i++) {
    int n = f12_write(f, lines[i], 2);
    ASSERT_EQ(n, 2);
  }

  f12_err_t err = f12_close(f);
  ASSERT_EQ(err, F12_OK);

  f = f12_open(&fs, "MULTI.TXT", "r");
  ASSERT(f != NULL);

  char buf[64];
  int n = f12_read(f, buf, sizeof(buf));
  ASSERT_EQ(n, 12);
  ASSERT_MEM_EQ(buf, "a\nb\nc\nd\ne\nf\n", 12);

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

static void list_counter(const f12_stat_t *stat, void *ctx) {
  (void)stat;
  int *count = (int *)ctx;
  (*count)++;
}

TEST(test_list_callback_proper) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  for (int i = 0; i < 5; i++) {
    char name[16];
    snprintf(name, sizeof(name), "F%d.TXT", i);
    f12_file_t *f = f12_open(&fs, name, "w");
    f12_write(f, "x", 1);
    f12_close(f);
  }

  int count = 0;
  f12_err_t err = f12_list(&fs, list_counter, &count);
  ASSERT_EQ(err, F12_OK);
  ASSERT_EQ(count, 5);

  f12_unmount(&fs);
}

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
  RUN_TEST(test_multiple_small_writes);
  RUN_TEST(test_strerror);
  RUN_TEST(test_list_callback_proper);

  TEST_RESULTS();
}
