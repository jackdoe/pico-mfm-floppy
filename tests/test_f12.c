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

typedef struct {
  vdisk_t *disk;
  int track_reads;
} failing_track_io_t;

static bool failing_track_read(void *ctx, sector_t *sector) {
  failing_track_io_t *io = (failing_track_io_t *)ctx;
  return vdisk_read(io->disk, sector);
}

static bool failing_track_read_track(void *ctx, track_t *track) {
  failing_track_io_t *io = (failing_track_io_t *)ctx;
  io->track_reads++;
  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    track->sectors[i].valid = true;
    memset(track->sectors[i].data, 0xEE, SECTOR_SIZE);
  }
  return false;
}

static bool failing_track_write(void *ctx, track_t *track) {
  failing_track_io_t *io = (failing_track_io_t *)ctx;
  return vdisk_write(io->disk, track);
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

TEST(test_mount_clears_uninitialized_state) {
  vdisk_format_valid(&vdisk);

  f12_t fs;
  memset(&fs, 0xA5, sizeof(fs));

  f12_err_t err = f12_mount(&fs, vdisk_f12_io());
  ASSERT_EQ(err, F12_OK);
  ASSERT(fs.mounted);
  ASSERT_NOT_NULL(fs.cache);

  f12_unmount(&fs);
}

TEST(test_failed_track_read_falls_back_to_sector_read) {
  vdisk_format_valid(&vdisk);

  failing_track_io_t fio = { .disk = &vdisk };
  f12_io_t io = {
    .read = failing_track_read,
    .read_track = failing_track_read_track,
    .write = failing_track_write,
    .disk_changed = NULL,
    .write_protected = NULL,
    .ctx = &fio,
  };

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  f12_err_t err = f12_mount(&fs, io);
  ASSERT_EQ(err, F12_OK);
  ASSERT(fio.track_reads > 0);
  ASSERT(vdisk.read_count > 0);

  f12_unmount(&fs);
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

TEST(test_disk_changed_aborts_pending_write) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "PEND.TXT", "w");
  ASSERT(f != NULL);
  ASSERT_EQ(f12_write(f, "hello", 5), 5);
  ASSERT(fs.fat.batch.active);

  vdisk.disk_changed = true;

  ASSERT_EQ(f12_write(f, "!", 1), -1);
  ASSERT_EQ(f12_errno(&fs), F12_ERR_DISK_CHANGED);
  ASSERT(!fs.mounted);
  ASSERT(!fs.fat.batch.active);
  ASSERT_EQ(f->mode, F12_MODE_CLOSED);

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
  ASSERT_EQ(f12_tell(f), 0);

  int n = f12_read_at(f, 4, buf, 4);
  ASSERT_EQ(n, 4);
  buf[4] = '\0';
  ASSERT_STR_EQ(buf, "BBBB");
  ASSERT_EQ(f12_tell(f), 0);

  n = f12_read_at(f, 12, buf, 4);
  ASSERT_EQ(n, 4);
  buf[4] = '\0';
  ASSERT_STR_EQ(buf, "DDDD");
  ASSERT_EQ(f12_tell(f), 0);

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

TEST(test_large_single_call_io) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  size_t size = 70000;
  uint8_t *write_buf = malloc(size);
  uint8_t *read_buf = malloc(size);
  ASSERT_NOT_NULL(write_buf);
  ASSERT_NOT_NULL(read_buf);

  for (size_t i = 0; i < size; i++) {
    write_buf[i] = (uint8_t)((i * 17u + 3u) & 0xFF);
  }

  f12_file_t *f = f12_open(&fs, "BIGCALL.BIN", "w");
  ASSERT(f != NULL);
  ASSERT_EQ(f12_write(f, write_buf, size), (int)size);
  ASSERT_EQ(f12_close(f), F12_OK);

  f12_stat_t stat;
  ASSERT_EQ(f12_stat(&fs, "BIGCALL.BIN", &stat), F12_OK);
  ASSERT_EQ(stat.size, size);

  f = f12_open(&fs, "BIGCALL.BIN", "r");
  ASSERT(f != NULL);
  ASSERT_EQ(f12_read(f, read_buf, size), (int)size);
  ASSERT_MEM_EQ(read_buf, write_buf, size);
  ASSERT_EQ(f12_close(f), F12_OK);

  free(read_buf);
  free(write_buf);
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

TEST(test_single_byte_writes) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "BYTES.BIN", "w");
  ASSERT(f != NULL);

  for (int i = 0; i < 256; i++) {
    uint8_t b = (uint8_t)i;
    int n = f12_write(f, &b, 1);
    ASSERT_EQ(n, 1);
  }

  f12_err_t err = f12_close(f);
  ASSERT_EQ(err, F12_OK);

  f = f12_open(&fs, "BYTES.BIN", "r");
  ASSERT(f != NULL);

  uint8_t buf[256];
  int n = f12_read(f, buf, sizeof(buf));
  ASSERT_EQ(n, 256);

  for (int i = 0; i < 256; i++) {
    if (buf[i] != (uint8_t)i) {
      printf("FAIL\n  Byte %d: expected %02X, got %02X\n", i, (uint8_t)i, buf[i]);
      exit(1);
    }
  }

  f12_close(f);
  f12_unmount(&fs);
}

TEST(test_rpc_chunk_writes) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "RPC.BIN", "w");
  ASSERT(f != NULL);

  uint32_t total = 10000;
  uint32_t written = 0;
  uint8_t chunk[121];
  while (written < total) {
    uint32_t len = total - written;
    if (len > 121) len = 121;
    for (uint32_t i = 0; i < len; i++)
      chunk[i] = (uint8_t)(written + i);
    int n = f12_write(f, chunk, len);
    ASSERT_EQ(n, (int)len);
    written += len;
  }

  f12_err_t err = f12_close(f);
  ASSERT_EQ(err, F12_OK);

  f12_stat_t stat;
  err = f12_stat(&fs, "RPC.BIN", &stat);
  ASSERT_EQ(err, F12_OK);
  ASSERT_EQ(stat.size, total);

  f = f12_open(&fs, "RPC.BIN", "r");
  ASSERT(f != NULL);

  uint8_t buf[512];
  uint32_t verified = 0;
  while (verified < total) {
    uint32_t want = total - verified;
    if (want > 512) want = 512;
    int n = f12_read(f, buf, want);
    ASSERT(n > 0);
    for (int i = 0; i < n; i++) {
      if (buf[i] != (uint8_t)(verified + i)) {
        printf("FAIL\n  Byte %lu: expected %02X, got %02X\n",
               (unsigned long)(verified + i), (uint8_t)(verified + i), buf[i]);
        exit(1);
      }
    }
    verified += n;
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

TEST(test_open_null_args) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  ASSERT_NULL(f12_open(&fs, NULL, "r"));
  ASSERT_EQ(f12_errno(&fs), F12_ERR_INVALID);
  ASSERT_NULL(f12_open(&fs, "X.TXT", NULL));
  ASSERT_EQ(f12_errno(&fs), F12_ERR_INVALID);
  ASSERT_NULL(f12_open(NULL, "X.TXT", "r"));

  f12_unmount(&fs);
}

TEST(test_open_invalid_mode) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());

  ASSERT_NULL(f12_open(&fs, "X.TXT", "z"));
  ASSERT_EQ(f12_errno(&fs), F12_ERR_INVALID);
  ASSERT_NULL(f12_open(&fs, "X.TXT", ""));

  f12_unmount(&fs);
}

TEST(test_op_on_unmounted) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "TEST", false);
  f12_mount(&fs, vdisk_f12_io());
  f12_unmount(&fs);

  ASSERT_NULL(f12_open(&fs, "ANY.TXT", "r"));
  ASSERT_EQ(f12_errno(&fs), F12_ERR_NOT_MOUNTED);

  f12_stat_t stat;
  ASSERT_EQ(f12_stat(&fs, "ANY.TXT", &stat), F12_ERR_NOT_MOUNTED);
  ASSERT_EQ(f12_delete(&fs, "ANY.TXT"), F12_ERR_NOT_MOUNTED);

  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), F12_ERR_NOT_MOUNTED);
}

TEST(test_format_with_null_write_callback) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = (f12_io_t){
    .read = vdisk_read,
    .write = NULL,
    .disk_changed = NULL,
    .write_protected = NULL,
    .ctx = &vdisk,
  };

  f12_err_t err = f12_format(&fs, "X", false);
  ASSERT_EQ(err, F12_ERR_INVALID);
  ASSERT_EQ(f12_errno(&fs), F12_ERR_INVALID);
}

TEST(test_format_write_protected) {
  vdisk_init(&vdisk);
  vdisk.write_protected = true;

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();

  f12_err_t err = f12_format(&fs, "X", false);
  ASSERT_EQ(err, F12_ERR_WRITE_PROTECTED);
}

TEST(test_format_remounts_when_already_mounted) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "OLD", false);
  f12_mount(&fs, vdisk_f12_io());
  ASSERT(fs.mounted);

  f12_err_t err = f12_format(&fs, "NEW", false);
  ASSERT_EQ(err, F12_OK);
  ASSERT(fs.mounted);

  f12_unmount(&fs);
}

TEST(test_read_in_write_mode_fails) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "WO.TXT", "w");
  ASSERT_NOT_NULL(f);
  char buf[8];
  ASSERT_EQ(f12_read(f, buf, sizeof(buf)), -1);
  ASSERT_EQ(f12_errno(&fs), F12_ERR_INVALID);
  f12_close(f);

  f12_unmount(&fs);
}

TEST(test_write_in_read_mode_fails) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "RO.TXT", "w");
  f12_write(f, "x", 1);
  f12_close(f);

  f = f12_open(&fs, "RO.TXT", "r");
  ASSERT_NOT_NULL(f);
  ASSERT_EQ(f12_write(f, "y", 1), -1);
  ASSERT_EQ(f12_errno(&fs), F12_ERR_INVALID);
  f12_close(f);

  f12_unmount(&fs);
}

TEST(test_seek_in_write_mode_fails) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "S.TXT", "w");
  ASSERT_NOT_NULL(f);
  ASSERT_EQ(f12_seek(f, 0), F12_ERR_INVALID);
  f12_close(f);

  f12_unmount(&fs);
}

TEST(test_tell_on_write_file) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "TELL.TXT", "w");
  ASSERT_EQ(f12_tell(f), 0);
  f12_write(f, "abc", 3);
  ASSERT_EQ(f12_tell(f), 3);
  f12_close(f);

  ASSERT_EQ(f12_tell(NULL), 0);

  f12_unmount(&fs);
}

TEST(test_delete_not_found) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  ASSERT_EQ(f12_delete(&fs, "GHOST.TXT"), F12_ERR_NOT_FOUND);

  f12_unmount(&fs);
}

TEST(test_opendir_non_root) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/sub", &dir), F12_ERR_NOT_FOUND);
  ASSERT_EQ(f12_opendir(&fs, "subdir", &dir), F12_ERR_NOT_FOUND);

  f12_unmount(&fs);
}

TEST(test_strerror_all_codes) {
  ASSERT_STR_EQ(f12_strerror(F12_OK), "Success");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_IO), "I/O error");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_NOT_FOUND), "File not found");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_EXISTS), "File exists");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_FULL), "Disk full");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_TOO_MANY), "Too many open files");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_INVALID), "Invalid argument");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_IS_DIR), "Is a directory");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_NOT_MOUNTED), "Not mounted");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_EOF), "End of file");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_DISK_CHANGED), "Disk changed");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_WRITE_PROTECTED), "Write protected");
  ASSERT_STR_EQ(f12_strerror(F12_ERR_BAD_HANDLE), "Bad file handle");
  ASSERT_STR_EQ(f12_strerror((f12_err_t)999), "Unknown error");
}

TEST(test_unmount_closes_open_files) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *fw = f12_open(&fs, "OPEN.W", "w");
  f12_write(fw, "abc", 3);

  f12_file_t *fr;
  {
    f12_file_t *tmp = f12_open(&fs, "OPEN.W", "w");
    ASSERT_NULL(tmp);
  }
  f12_close(fw);

  fw = f12_open(&fs, "READ.TXT", "w");
  f12_write(fw, "data", 4);
  f12_close(fw);

  fr = f12_open(&fs, "READ.TXT", "r");
  ASSERT_NOT_NULL(fr);
  ASSERT_EQ(fr->mode, F12_MODE_READ);

  f12_unmount(&fs);
  ASSERT_EQ(fr->mode, F12_MODE_CLOSED);
}

TEST(test_seek_clamped_past_eof) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "SK.TXT", "w");
  f12_write(f, "0123456789", 10);
  f12_close(f);

  f = f12_open(&fs, "SK.TXT", "r");
  ASSERT_NOT_NULL(f);

  ASSERT_EQ(f12_seek(f, 9999), F12_OK);
  ASSERT_EQ(f12_tell(f), 10);
  char buf[8];
  ASSERT_EQ(f12_read(f, buf, sizeof(buf)), 0);

  f12_close(f);
  f12_unmount(&fs);
}

TEST(test_close_invalid_handle) {
  ASSERT_EQ(f12_close(NULL), F12_ERR_BAD_HANDLE);

  f12_file_t orphan;
  memset(&orphan, 0, sizeof(orphan));
  ASSERT_EQ(f12_close(&orphan), F12_ERR_BAD_HANDLE);
}

TEST(test_seek_null_handle) {
  ASSERT_EQ(f12_seek(NULL, 0), F12_ERR_BAD_HANDLE);
}

TEST(test_read_negative_args) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "X.TXT", "w");
  f12_write(f, "z", 1);
  f12_close(f);

  f = f12_open(&fs, "X.TXT", "r");
  ASSERT_NOT_NULL(f);

  ASSERT_EQ(f12_read(NULL, NULL, 0), -1);
  ASSERT_EQ(f12_read(f, NULL, 0), -1);

  f12_close(f);

  ASSERT_EQ(f12_write(NULL, NULL, 0), -1);

  f12_unmount(&fs);
}

TEST(test_readdir_eof_after_all) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(&fs, "/", &dir), F12_OK);

  f12_stat_t stat;
  while (f12_readdir(&dir, &stat) == F12_OK) {}

  ASSERT_EQ(f12_readdir(&dir, &stat), F12_ERR_EOF);
  ASSERT_EQ(f12_closedir(&dir), F12_OK);

  f12_unmount(&fs);
}

TEST(test_errno_null_fs) {
  ASSERT_EQ(f12_errno(NULL), F12_ERR_INVALID);
}

TEST(test_io_callbacks_null_optional) {
  vdisk_init(&vdisk);

  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  f12_io_t io = {
    .read = vdisk_read,
    .write = vdisk_write,
    .disk_changed = NULL,
    .write_protected = NULL,
    .ctx = &vdisk,
  };

  fs.io = io;
  ASSERT_EQ(f12_format(&fs, "T", false), F12_OK);
  ASSERT_EQ(f12_mount(&fs, io), F12_OK);

  f12_file_t *f = f12_open(&fs, "X.TXT", "w");
  ASSERT_NOT_NULL(f);
  ASSERT_EQ(f12_write(f, "hi", 2), 2);
  ASSERT_EQ(f12_close(f), F12_OK);

  f = f12_open(&fs, "X.TXT", "r");
  ASSERT_NOT_NULL(f);
  char buf[8];
  ASSERT_EQ(f12_read(f, buf, sizeof(buf)), 2);
  f12_close(f);

  f12_unmount(&fs);
}

TEST(test_seek_propagates_error) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "S.TXT", "w");
  uint8_t buf[1500];
  memset(buf, 0xCD, sizeof(buf));
  f12_write(f, buf, sizeof(buf));
  f12_close(f);

  f = f12_open(&fs, "S.TXT", "r");
  ASSERT_NOT_NULL(f);

  ASSERT_EQ(f12_seek(f, 100), F12_OK);
  ASSERT_EQ(f12_tell(f), 100);

  f12_close(f);
  f12_unmount(&fs);
}

TEST(test_read_at_seek_back_after_failure) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "RA.TXT", "w");
  uint8_t buf[256];
  memset(buf, 0xEE, sizeof(buf));
  f12_write(f, buf, sizeof(buf));
  f12_close(f);

  f = f12_open(&fs, "RA.TXT", "r");
  ASSERT_NOT_NULL(f);

  uint8_t out[16];
  int n = f12_read_at(f, 50, out, sizeof(out));
  ASSERT_EQ(n, (int)sizeof(out));
  ASSERT_EQ(f12_tell(f), 0);

  ASSERT_EQ(f12_read_at(NULL, 0, out, 1), -1);

  f12_close(f);
  f12_unmount(&fs);
}

TEST(test_close_already_closed_is_safe) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "C.TXT", "w");
  f12_write(f, "x", 1);
  ASSERT_EQ(f12_close(f), F12_OK);
  ASSERT_EQ(f12_close(f), F12_OK);

  f12_unmount(&fs);
}

TEST(test_open_directory_returns_is_dir) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  uint16_t root_lba = fs.fat.root_dir_start_sector;
  fat12_dirent_t entry;
  memset(&entry, 0, sizeof(entry));
  memcpy(entry.name, "SUBDIR  ", 8);
  memcpy(entry.ext, "   ", 3);
  entry.attr = FAT12_ATTR_DIRECTORY;
  entry.start_cluster = 2;
  entry.size = 0;
  memcpy(&vdisk.data[root_lba][0], &entry, sizeof(entry));

  f12_file_t *f = f12_open(&fs, "SUBDIR", "r");
  ASSERT_NULL(f);
  ASSERT_EQ(f12_errno(&fs), F12_ERR_IS_DIR);

  f12_unmount(&fs);
}

TEST(test_read_propagates_fat_error) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_file_t *f = f12_open(&fs, "B.TXT", "w");
  uint8_t buf[2000];
  memset(buf, 0xBB, sizeof(buf));
  f12_write(f, buf, sizeof(buf));
  f12_close(f);

  fat12_dirent_t e;
  ASSERT_EQ(fat12_find(&fs.fat, "B.TXT", &e), FAT12_OK);
  uint16_t start = e.start_cluster;

  vdisk_set_fat_entry(&vdisk, start, 0xFF7);

  f = f12_open(&fs, "B.TXT", "r");
  ASSERT_NOT_NULL(f);
  uint8_t out[2000];
  int n = f12_read(f, out, sizeof(out));
  ASSERT(n >= 0);

  f12_close(f);
  f12_unmount(&fs);
}

TEST(test_stat_null_args) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "T", false);
  f12_mount(&fs, vdisk_f12_io());

  f12_stat_t stat;
  ASSERT_EQ(f12_stat(NULL, "X.TXT", &stat), F12_ERR_INVALID);
  ASSERT_EQ(f12_stat(&fs, NULL, &stat), F12_ERR_INVALID);
  ASSERT_EQ(f12_stat(&fs, "X.TXT", NULL), F12_ERR_INVALID);

  ASSERT_EQ(f12_delete(NULL, "X.TXT"), F12_ERR_INVALID);
  ASSERT_EQ(f12_delete(&fs, NULL), F12_ERR_INVALID);

  f12_dir_t dir;
  ASSERT_EQ(f12_opendir(NULL, "/", &dir), F12_ERR_INVALID);
  ASSERT_EQ(f12_opendir(&fs, NULL, &dir), F12_ERR_INVALID);
  ASSERT_EQ(f12_opendir(&fs, "/", NULL), F12_ERR_INVALID);

  ASSERT_EQ(f12_readdir(NULL, &stat), F12_ERR_INVALID);
  ASSERT_EQ(f12_closedir(NULL), F12_ERR_INVALID);

  ASSERT_EQ(f12_list(NULL, NULL, NULL), F12_ERR_INVALID);
  ASSERT_EQ(f12_list(&fs, NULL, NULL), F12_ERR_INVALID);

  f12_unmount(&fs);
}

TEST(test_fat2_sectors_not_pinned) {
  vdisk_init(&vdisk);
  f12_t fs;
  memset(&fs, 0, sizeof(fs));
  fs.io = vdisk_f12_io();
  f12_format(&fs, "PINTEST", true);
  ASSERT_EQ(f12_mount(&fs, vdisk_f12_io()), F12_OK);

  uint8_t buf[4096];
  for (int i = 0; i < (int)sizeof(buf); i++) buf[i] = (uint8_t)(i * 31 + 7);
  f12_file_t *f = f12_open(&fs, "DATA.BIN", "w");
  ASSERT_NOT_NULL(f);
  f12_write(f, buf, sizeof(buf));
  ASSERT_EQ(f12_close(f), F12_OK);
  f = f12_open(&fs, "DATA.BIN", "r");
  ASSERT_NOT_NULL(f);
  f12_read(f, buf, sizeof(buf));
  f12_close(f);

  uint16_t fat2_lo = fs.fat.fat_start_sector + fs.fat.bpb.sectors_per_fat;
  uint16_t fat2_hi = fat2_lo + fs.fat.bpb.sectors_per_fat;
  uint16_t fat1_lo = fs.fat.fat_start_sector;

  lru_t *c = fs.cache;
  int pinned_fat2 = 0, pinned_fat1 = 0;
  for (uint32_t i = 0; i < c->max_entries; i++) {
    lru_entry_t *e = (lru_entry_t *)(c->storage + i * c->entry_stride);
    if (!e->occupied || !e->pinned) continue;
    uint8_t track = (e->key >> 16) & 0xFF;
    uint8_t side = (e->key >> 8) & 0xFF;
    uint8_t sec = e->key & 0xFF;
    uint16_t lba = (track * 2 + side) * SECTORS_PER_TRACK + (sec - 1);
    if (lba >= fat2_lo && lba < fat2_hi) pinned_fat2++;
    if (lba >= fat1_lo && lba < fat2_lo) pinned_fat1++;
  }

  ASSERT_EQ(pinned_fat2, 0);
  ASSERT(pinned_fat1 > 0);

  f12_unmount(&fs);
}

int main(void) {
  printf("=== F12 High-Level API Tests ===\n\n");

  RUN_TEST(test_mount_unmount);
  RUN_TEST(test_mount_clears_uninitialized_state);
  RUN_TEST(test_failed_track_read_falls_back_to_sector_read);
  RUN_TEST(test_format_and_mount);
  RUN_TEST(test_create_write_read_file);
  RUN_TEST(test_file_stat);
  RUN_TEST(test_file_delete);
  RUN_TEST(test_directory_listing);
  RUN_TEST(test_too_many_open_files);
  RUN_TEST(test_write_protected);
  RUN_TEST(test_disk_changed);
  RUN_TEST(test_disk_changed_aborts_pending_write);
  RUN_TEST(test_seek_and_tell);
  RUN_TEST(test_read_at);
  RUN_TEST(test_file_not_found);
  RUN_TEST(test_large_file);
  RUN_TEST(test_large_single_call_io);
  RUN_TEST(test_multiple_small_writes);
  RUN_TEST(test_single_byte_writes);
  RUN_TEST(test_rpc_chunk_writes);
  RUN_TEST(test_strerror);
  RUN_TEST(test_list_callback_proper);
  RUN_TEST(test_open_null_args);
  RUN_TEST(test_open_invalid_mode);
  RUN_TEST(test_op_on_unmounted);
  RUN_TEST(test_format_with_null_write_callback);
  RUN_TEST(test_format_write_protected);
  RUN_TEST(test_format_remounts_when_already_mounted);
  RUN_TEST(test_read_in_write_mode_fails);
  RUN_TEST(test_write_in_read_mode_fails);
  RUN_TEST(test_seek_in_write_mode_fails);
  RUN_TEST(test_tell_on_write_file);
  RUN_TEST(test_delete_not_found);
  RUN_TEST(test_opendir_non_root);
  RUN_TEST(test_strerror_all_codes);
  RUN_TEST(test_unmount_closes_open_files);
  RUN_TEST(test_seek_clamped_past_eof);
  RUN_TEST(test_close_invalid_handle);
  RUN_TEST(test_seek_null_handle);
  RUN_TEST(test_read_negative_args);
  RUN_TEST(test_readdir_eof_after_all);
  RUN_TEST(test_errno_null_fs);
  RUN_TEST(test_io_callbacks_null_optional);
  RUN_TEST(test_seek_propagates_error);
  RUN_TEST(test_read_at_seek_back_after_failure);
  RUN_TEST(test_close_already_closed_is_safe);
  RUN_TEST(test_open_directory_returns_is_dir);
  RUN_TEST(test_read_propagates_fat_error);
  RUN_TEST(test_stat_null_args);
  RUN_TEST(test_fat2_sectors_not_pinned);

  TEST_RESULTS();
}
