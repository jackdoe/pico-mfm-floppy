#include "test.h"
#include "vdisk.h"
#include "../src/fat12.h"

typedef struct {
  vdisk_t disk;
  int fail_after_reads;
  int fail_after_track_writes;
} fault_vdisk_t;

static bool fault_vdisk_read(void *ctx, sector_t *sector) {
  fault_vdisk_t *disk = (fault_vdisk_t *)ctx;
  if (disk->fail_after_reads == 0) {
    sector->valid = false;
    return false;
  }
  if (disk->fail_after_reads > 0) {
    disk->fail_after_reads--;
  }
  return vdisk_read(&disk->disk, sector);
}

static bool fault_vdisk_write(void *ctx, track_t *track) {
  fault_vdisk_t *disk = (fault_vdisk_t *)ctx;
  if (disk->fail_after_track_writes == 0) {
    return false;
  }
  if (disk->fail_after_track_writes > 0) {
    disk->fail_after_track_writes--;
  }
  return vdisk_write(&disk->disk, track);
}

static uint16_t count_free_clusters(fat12_t *fat) {
  uint16_t free_clusters = 0;

  for (uint16_t cluster = 2; cluster < fat->total_clusters + 2; cluster++) {
    uint16_t next = 0;
    ASSERT_EQ(fat12_get_entry(fat, cluster, &next), FAT12_OK);
    if (next == 0) {
      free_clusters++;
    }
  }

  return free_clusters;
}

TEST(test_init) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };

  fat12_err_t err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_OK);
  ASSERT_EQ(fat.bpb.bytes_per_sector, 512);
  ASSERT_EQ(fat.bpb.sectors_per_cluster, 1);
  ASSERT_EQ(fat.bpb.num_fats, 2);
  ASSERT_EQ(fat.bpb.root_entries, 224);
  ASSERT_EQ(fat.bpb.sectors_per_track, 18);
  ASSERT_EQ(fat.bpb.num_heads, 2);
}

TEST(test_empty_directory) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_dirent_t entry;
  fat12_err_t err = fat12_find(&fat, "NOFILE.TXT", &entry);
  ASSERT_EQ(err, FAT12_ERR_NOT_FOUND);
}

TEST(test_create_file) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_dirent_t entry;
  fat12_err_t err = fat12_create(&fat, "TEST.TXT", &entry);
  ASSERT_EQ(err, FAT12_OK);

  fat12_dirent_t found;
  err = fat12_find(&fat, "TEST.TXT", &found);
  ASSERT_EQ(err, FAT12_OK);
  ASSERT_MEM_EQ(found.name, "TEST    ", 8);
  ASSERT_MEM_EQ(found.ext, "TXT", 3);
  ASSERT_EQ(found.size, 0);
}

TEST(test_create_propagates_find_error) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_dirent_t entry;
  disk.fail_after_reads = 0;

  fat12_err_t err = fat12_create(&fat, "FAIL.TXT", &entry);
  ASSERT_EQ(err, FAT12_ERR_READ);
  ASSERT_EQ(disk.disk.track_writes, 0);
  ASSERT_EQ(disk.disk.write_count, 0);
  ASSERT_EQ((uint8_t)disk.disk.data[19][0], FAT12_DIRENT_END);
}

TEST(test_open_write_failure_cleans_up_batch) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  disk.fail_after_reads = 0;

  fat12_err_t err = fat12_open_write(&fat, "BROKEN.TXT", &writer);
  ASSERT_EQ(err, FAT12_ERR_READ);
  ASSERT(!fat.batch.active);
  ASSERT_NULL(writer.fat);

  disk.fail_after_reads = -1;
  err = fat12_open_write(&fat, "GOOD.TXT", &writer);
  ASSERT_EQ(err, FAT12_OK);
  ASSERT_EQ(fat12_write(&writer, (const uint8_t *)"ok", 2), 2);
  ASSERT_EQ(fat12_close_write(&writer), FAT12_OK);
}

TEST(test_write_small_file) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  fat12_err_t err = fat12_open_write(&fat, "HELLO.TXT", &writer);
  ASSERT_EQ(err, FAT12_OK);

  const char *msg = "Hello, World!";
  int written = fat12_write(&writer, (uint8_t *)msg, strlen(msg));
  ASSERT_EQ(written, (int)strlen(msg));

  err = fat12_close_write(&writer);
  ASSERT_EQ(err, FAT12_OK);

  fat12_dirent_t entry;
  err = fat12_find(&fat, "HELLO.TXT", &entry);
  ASSERT_EQ(err, FAT12_OK);
  ASSERT_EQ(entry.size, strlen(msg));

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);

  char buf[64] = {0};
  int n = fat12_read(&file, (uint8_t *)buf, sizeof(buf));
  ASSERT_EQ(n, (int)strlen(msg));
  ASSERT_MEM_EQ(buf, msg, strlen(msg));
}

TEST(test_small_reads_use_cluster_buffer) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "READBUF.TXT", &writer), FAT12_OK);

  const char *msg = "Buffered single-byte reads should only hit the disk once per cluster.";
  ASSERT_EQ(fat12_write(&writer, (const uint8_t *)msg, strlen(msg)), (int)strlen(msg));
  ASSERT_EQ(fat12_close_write(&writer), FAT12_OK);

  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "READBUF.TXT", &entry), FAT12_OK);

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);
  disk.read_count = 0;

  for (size_t i = 0; i < strlen(msg); i++) {
    char ch = 0;
    ASSERT_EQ(fat12_read(&file, (uint8_t *)&ch, 1), 1);
    ASSERT_EQ(ch, msg[i]);
  }

  ASSERT_EQ(disk.read_count, 1);
}

TEST(test_write_large_file) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  fat12_err_t err = fat12_open_write(&fat, "BIG.DAT", &writer);
  ASSERT_EQ(err, FAT12_OK);

  uint8_t pattern[2000];
  for (int i = 0; i < 2000; i++) {
    pattern[i] = i & 0xFF;
  }

  int written = fat12_write(&writer, pattern, sizeof(pattern));
  ASSERT_EQ(written, 2000);

  err = fat12_close_write(&writer);
  ASSERT_EQ(err, FAT12_OK);

  fat12_dirent_t entry;
  err = fat12_find(&fat, "BIG.DAT", &entry);
  ASSERT_EQ(err, FAT12_OK);
  ASSERT_EQ(entry.size, 2000);

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);

  uint8_t buf[2000];
  int n = fat12_read(&file, buf, sizeof(buf));
  ASSERT_EQ(n, 2000);

  for (int i = 0; i < 2000; i++) {
    if (buf[i] != (i & 0xFF)) {
      printf("FAIL\n  Data mismatch at byte %d: expected %02X, got %02X\n",
             i, i & 0xFF, buf[i]);
      exit(1);
    }
  }
}

TEST(test_write_read_large_single_call) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  size_t size = 70000;
  uint8_t *pattern = malloc(size);
  uint8_t *buf = malloc(size);
  ASSERT_NOT_NULL(pattern);
  ASSERT_NOT_NULL(buf);

  for (size_t i = 0; i < size; i++) {
    pattern[i] = (uint8_t)((i * 13u + 7u) & 0xFF);
  }

  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "BIGCALL.BIN", &writer), FAT12_OK);
  ASSERT_EQ(fat12_write(&writer, pattern, size), (int)size);
  ASSERT_EQ(fat12_close_write(&writer), FAT12_OK);

  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "BIGCALL.BIN", &entry), FAT12_OK);
  ASSERT_EQ(entry.size, size);

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);
  ASSERT_EQ(fat12_read(&file, buf, size), (int)size);
  ASSERT_MEM_EQ(buf, pattern, size);

  free(buf);
  free(pattern);
}

TEST(test_overwrite_file) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  fat12_open_write(&fat, "DATA.TXT", &writer);
  fat12_write(&writer, (uint8_t *)"First version", 13);
  fat12_close_write(&writer);

  fat12_open_write(&fat, "DATA.TXT", &writer);
  fat12_write(&writer, (uint8_t *)"Second", 6);
  fat12_close_write(&writer);

  fat12_dirent_t entry;
  fat12_find(&fat, "DATA.TXT", &entry);
  ASSERT_EQ(entry.size, 6);

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);

  char buf[64] = {0};
  fat12_read(&file, (uint8_t *)buf, sizeof(buf));
  ASSERT_MEM_EQ(buf, "Second", 6);
}

TEST(test_overwrite_failure_preserves_existing_file) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "SAFE.TXT", &writer), FAT12_OK);
  ASSERT_EQ(fat12_write(&writer, (const uint8_t *)"old data", 8), 8);
  ASSERT_EQ(fat12_close_write(&writer), FAT12_OK);

  disk.fail_after_track_writes = 0;

  ASSERT_EQ(fat12_open_write(&fat, "SAFE.TXT", &writer), FAT12_OK);
  ASSERT_EQ(fat12_write(&writer, (const uint8_t *)"new data that should not stick", 30), 30);
  ASSERT_EQ(fat12_close_write(&writer), FAT12_ERR_WRITE);
  ASSERT(!fat.batch.active);

  disk.fail_after_track_writes = -1;

  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "SAFE.TXT", &entry), FAT12_OK);
  ASSERT_EQ(entry.size, 8);

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);
  char buf[32] = {0};
  ASSERT_EQ(fat12_read(&file, (uint8_t *)buf, sizeof(buf)), 8);
  ASSERT_MEM_EQ(buf, "old data", 8);
}

TEST(test_invalid_83_names_rejected) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  ASSERT_EQ(fat12_init(&fat, io), FAT12_OK);

  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "", &writer), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_open_write(&fat, "TOOLONGNAME.TXT", &writer), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_open_write(&fat, "BAD.LONG", &writer), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_open_write(&fat, "A.B.C", &writer), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_open_write(&fat, "BAD NAME.TXT", &writer), FAT12_ERR_INVALID);

  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "BAD NAME.TXT", &entry), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_create(&fat, "TOOLONGNAME.TXT", &entry), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_delete(&fat, "BAD NAME.TXT"), FAT12_ERR_INVALID);
  ASSERT(!fat.batch.active);
}

TEST(test_delete_marks_dirent_before_freeing_chain) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  ASSERT_EQ(fat12_init(&fat, io), FAT12_OK);

  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "DROP.TXT", &writer), FAT12_OK);
  uint8_t data[700];
  memset(data, 0x31, sizeof(data));
  ASSERT_EQ(fat12_write(&writer, data, sizeof(data)), (int)sizeof(data));
  ASSERT_EQ(fat12_close_write(&writer), FAT12_OK);

  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "DROP.TXT", &entry), FAT12_OK);
  uint16_t start = entry.start_cluster;
  ASSERT(start >= 2);

  disk.fail_after_track_writes = 1;
  ASSERT_EQ(fat12_delete(&fat, "DROP.TXT"), FAT12_ERR_WRITE);
  ASSERT(!fat.batch.active);

  disk.fail_after_track_writes = -1;
  ASSERT_EQ(fat12_init(&fat, io), FAT12_OK);
  ASSERT_EQ(fat12_find(&fat, "DROP.TXT", &entry), FAT12_ERR_NOT_FOUND);

  uint16_t next = 0;
  ASSERT_EQ(fat12_get_entry(&fat, start, &next), FAT12_OK);
  ASSERT(next != 0);
}

TEST(test_overwrite_succeeds_without_spare_clusters) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  uint8_t original[1024];
  uint8_t updated[1024];
  for (size_t i = 0; i < sizeof(original); i++) {
    original[i] = (uint8_t)(i & 0xFF);
    updated[i] = (uint8_t)(0xFF - (i & 0xFF));
  }

  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "TARGET.BIN", &writer), FAT12_OK);
  ASSERT_EQ(fat12_write(&writer, original, sizeof(original)), (int)sizeof(original));
  ASSERT_EQ(fat12_close_write(&writer), FAT12_OK);

  uint16_t free_clusters = count_free_clusters(&fat);
  ASSERT(free_clusters > 0);

  uint8_t cluster_buf[FAT12_MAX_CLUSTER_SECTORS * SECTOR_SIZE];
  memset(cluster_buf, 0xA5, sizeof(cluster_buf));

  ASSERT_EQ(fat12_open_write(&fat, "FILLER.BIN", &writer), FAT12_OK);
  size_t cluster_size = fat.bpb.sectors_per_cluster * SECTOR_SIZE;
  for (uint16_t i = 0; i < free_clusters; i++) {
    ASSERT_EQ(fat12_write(&writer, cluster_buf, cluster_size), (int)cluster_size);
  }
  ASSERT_EQ(fat12_close_write(&writer), FAT12_OK);
  ASSERT_EQ(count_free_clusters(&fat), 0);

  ASSERT_EQ(fat12_open_write(&fat, "TARGET.BIN", &writer), FAT12_OK);
  ASSERT_EQ(fat12_write(&writer, updated, sizeof(updated)), (int)sizeof(updated));
  ASSERT_EQ(fat12_close_write(&writer), FAT12_OK);

  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "TARGET.BIN", &entry), FAT12_OK);
  ASSERT_EQ(entry.size, sizeof(updated));

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);
  uint8_t buf[sizeof(updated)];
  ASSERT_EQ(fat12_read(&file, buf, sizeof(buf)), (int)sizeof(buf));
  ASSERT_MEM_EQ(buf, updated, sizeof(updated));
}

TEST(test_delete_file) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  fat12_open_write(&fat, "DELETE.ME", &writer);
  fat12_write(&writer, (uint8_t *)"To be deleted", 13);
  fat12_close_write(&writer);

  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "DELETE.ME", &entry), FAT12_OK);

  fat12_err_t err = fat12_delete(&fat, "DELETE.ME");
  ASSERT_EQ(err, FAT12_OK);

  err = fat12_find(&fat, "DELETE.ME", &entry);
  ASSERT_EQ(err, FAT12_ERR_NOT_FOUND);
}

TEST(test_multiple_files) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  const char *names[] = {"FILE1.TXT", "FILE2.TXT", "FILE3.TXT", "DATA.BIN"};
  const char *contents[] = {"Content 1", "Content 2", "Content 3", "Binary"};

  for (int i = 0; i < 4; i++) {
    fat12_writer_t writer;
    fat12_open_write(&fat, names[i], &writer);
    fat12_write(&writer, (uint8_t *)contents[i], strlen(contents[i]));
    fat12_close_write(&writer);
  }

  for (int i = 0; i < 4; i++) {
    fat12_dirent_t entry;
    fat12_err_t err = fat12_find(&fat, names[i], &entry);
    ASSERT_EQ(err, FAT12_OK);
    ASSERT_EQ(entry.size, strlen(contents[i]));

    fat12_file_t file;
    fat12_open(&fat, &entry, &file);

    char buf[64] = {0};
    fat12_read(&file, (uint8_t *)buf, sizeof(buf));
    ASSERT_MEM_EQ(buf, contents[i], strlen(contents[i]));
  }
}

TEST(test_case_insensitive) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  fat12_open_write(&fat, "UPPER.TXT", &writer);
  fat12_write(&writer, (uint8_t *)"test", 4);
  fat12_close_write(&writer);

  fat12_dirent_t entry;
  fat12_err_t err = fat12_find(&fat, "upper.txt", &entry);
  ASSERT_EQ(err, FAT12_OK);

  err = fat12_find(&fat, "Upper.Txt", &entry);
  ASSERT_EQ(err, FAT12_OK);
}

TEST(test_batching_efficiency) {
  vdisk_t disk;
  vdisk_format_valid(&disk);
  disk.track_writes = 0;

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  fat12_open_write(&fat, "BATCH.DAT", &writer);

  uint8_t data[5000];
  memset(data, 0xAA, sizeof(data));
  fat12_write(&writer, data, sizeof(data));
  fat12_close_write(&writer);

  printf("\n  Track writes: %d (sectors written: %d)\n  ",
         disk.track_writes, disk.write_count);

  ASSERT(disk.track_writes <= 6);
}

TEST(test_write_read_cycle) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  for (int cycle = 0; cycle < 3; cycle++) {
    char filename[16];
    sprintf(filename, "CYCLE%d.DAT", cycle);

    fat12_writer_t writer;
    fat12_open_write(&fat, filename, &writer);

    uint8_t data[1024];
    for (int i = 0; i < 1024; i++) {
      data[i] = (cycle * 100 + i) & 0xFF;
    }
    fat12_write(&writer, data, sizeof(data));
    fat12_close_write(&writer);

    fat12_dirent_t entry;
    fat12_find(&fat, filename, &entry);

    fat12_file_t file;
    fat12_open(&fat, &entry, &file);

    uint8_t buf[1024];
    fat12_read(&file, buf, sizeof(buf));

    for (int i = 0; i < 1024; i++) {
      uint8_t expected = (cycle * 100 + i) & 0xFF;
      if (buf[i] != expected) {
        printf("FAIL\n  Cycle %d, byte %d: expected %02X, got %02X\n",
               cycle, i, expected, buf[i]);
        exit(1);
      }
    }
  }
}

TEST(test_cluster_chain) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  fat12_open_write(&fat, "CHAIN.DAT", &writer);

  uint8_t data[3072];
  for (int i = 0; i < 3072; i++) {
    data[i] = i & 0xFF;
  }
  fat12_write(&writer, data, sizeof(data));
  fat12_close_write(&writer);

  fat12_dirent_t entry;
  fat12_find(&fat, "CHAIN.DAT", &entry);
  ASSERT_EQ(entry.size, 3072);
  ASSERT(entry.start_cluster >= 2);

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);

  uint8_t buf[3072];
  int total = 0;
  int n;
  while ((n = fat12_read(&file, buf + total, 512)) > 0) {
    total += n;
  }
  ASSERT_EQ(total, 3072);

  for (int i = 0; i < 3072; i++) {
    if (buf[i] != (i & 0xFF)) {
      printf("FAIL\n  Byte %d: expected %02X, got %02X\n", i, i & 0xFF, buf[i]);
      exit(1);
    }
  }
}

TEST(test_reuse_deleted_entry) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  fat12_open_write(&fat, "FIRST.TXT", &writer);
  fat12_write(&writer, (uint8_t *)"First", 5);
  fat12_close_write(&writer);

  fat12_delete(&fat, "FIRST.TXT");

  fat12_open_write(&fat, "SECOND.TXT", &writer);
  fat12_write(&writer, (uint8_t *)"Second", 6);
  fat12_close_write(&writer);

  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "SECOND.TXT", &entry), FAT12_OK);
  ASSERT_EQ(entry.size, 6);
}

TEST(test_fat_entry_manipulation) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  fat12_open_write(&fat, "MULTI.DAT", &writer);

  uint8_t data[2048];
  memset(data, 0x55, sizeof(data));
  fat12_write(&writer, data, sizeof(data));
  fat12_close_write(&writer);

  fat12_dirent_t entry;
  fat12_find(&fat, "MULTI.DAT", &entry);

  uint16_t cluster = entry.start_cluster;
  int chain_length = 0;

  while (cluster >= 2 && !fat12_is_eof(cluster) && chain_length < 10) {
    chain_length++;
    uint16_t next = 0;
    fat12_get_entry(&fat, cluster, &next);
    cluster = next;
  }

  ASSERT_EQ(chain_length, 4);
}

TEST(test_format_quick) {
  vdisk_t disk;
  memset(&disk, 0xFF, sizeof(disk));
  disk.track_writes = 0;
  disk.read_count = 0;
  disk.write_count = 0;
  disk.write_protected = false;
  disk.disk_changed = false;

  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };

  fat12_err_t err = fat12_format(io, "TESTDISK", false);
  ASSERT_EQ(err, FAT12_OK);

  ASSERT(disk.track_writes <= 4);

  uint8_t *boot = disk.data[0];
  ASSERT_EQ(boot[0], 0xEB);
  ASSERT_EQ(boot[510], 0x55);
  ASSERT_EQ(boot[511], 0xAA);
  ASSERT_EQ(boot[11] | (boot[12] << 8), 512);
  ASSERT_EQ(boot[13], 1);
  ASSERT_EQ(boot[16], 2);
  ASSERT_EQ(boot[21], 0xF0);

  ASSERT_EQ(disk.data[1][0], 0xF0);
  ASSERT_EQ(disk.data[1][1], 0xFF);
  ASSERT_EQ(disk.data[1][2], 0xFF);

  ASSERT_EQ(disk.data[10][0], 0xF0);
  ASSERT_EQ(disk.data[10][1], 0xFF);
  ASSERT_EQ(disk.data[10][2], 0xFF);

  fat12_dirent_t *label = (fat12_dirent_t *)disk.data[19];
  ASSERT(memcmp(label->name, "TESTDISK", 8) == 0);
  ASSERT_EQ(label->attr, FAT12_ATTR_VOLUME_ID);
}

TEST(test_format_full) {
  vdisk_t disk;
  memset(&disk, 0xFF, sizeof(disk));
  disk.track_writes = 0;
  disk.read_count = 0;
  disk.write_count = 0;
  disk.write_protected = false;
  disk.disk_changed = false;

  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };

  fat12_err_t err = fat12_format(io, "FULLDISK", true);
  ASSERT_EQ(err, FAT12_OK);

  ASSERT_EQ(disk.track_writes, 160);

  ASSERT_EQ(disk.data[33][0], 0);
  ASSERT_EQ(disk.data[2879][0], 0);
}

static int g_prog_calls;
static uint16_t g_prog_last_done;
static uint16_t g_prog_last_total;
static uint16_t g_prog_prev_done;
static bool g_prog_monotonic;

static void record_progress(void *ctx, uint8_t cyl, uint8_t side,
                            uint16_t done, uint16_t total) {
  (void)ctx; (void)cyl; (void)side;
  if (done != g_prog_prev_done + 1) g_prog_monotonic = false;
  g_prog_prev_done = done;
  g_prog_calls++;
  g_prog_last_done = done;
  g_prog_last_total = total;
}

TEST(test_format_progress_callback) {
  vdisk_t disk;
  vdisk_init(&disk);
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write,
                    .progress = record_progress, .ctx = &disk };

  g_prog_calls = 0; g_prog_prev_done = 0; g_prog_monotonic = true;
  ASSERT_EQ(fat12_format(io, "PROG", true), FAT12_OK);
  ASSERT_EQ(g_prog_calls, 160);
  ASSERT_EQ(g_prog_last_total, 160);
  ASSERT_EQ(g_prog_last_done, 160);
  ASSERT(g_prog_monotonic);

  vdisk_init(&disk);
  g_prog_calls = 0; g_prog_prev_done = 0; g_prog_monotonic = true;
  ASSERT_EQ(fat12_format(io, "PROG", false), FAT12_OK);
  ASSERT(g_prog_calls > 0);
  ASSERT_EQ(g_prog_last_done, g_prog_last_total);
  ASSERT(g_prog_monotonic);
}

TEST(test_format_no_label) {
  vdisk_t disk;
  vdisk_init(&disk);

  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };

  fat12_err_t err = fat12_format(io, NULL, false);
  ASSERT_EQ(err, FAT12_OK);

  uint8_t *boot = disk.data[0];
  ASSERT(memcmp(&boot[43], "NO NAME    ", 11) == 0);

  fat12_dirent_t *first_entry = (fat12_dirent_t *)disk.data[19];
  ASSERT_EQ((uint8_t)first_entry->name[0], 0);
}

TEST(test_format_then_init) {
  vdisk_t disk;
  vdisk_init(&disk);

  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };

  fat12_err_t err = fat12_format(io, "MYDISK", false);
  ASSERT_EQ(err, FAT12_OK);

  fat12_t fat;
  err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_OK);

  ASSERT_EQ(fat.bpb.bytes_per_sector, 512);
  ASSERT_EQ(fat.bpb.sectors_per_cluster, 1);
  ASSERT_EQ(fat.bpb.reserved_sectors, 1);
  ASSERT_EQ(fat.bpb.num_fats, 2);
  ASSERT_EQ(fat.bpb.root_entries, 224);
  ASSERT_EQ(fat.bpb.total_sectors, 2880);
  ASSERT_EQ(fat.bpb.media_descriptor, 0xF0);
  ASSERT_EQ(fat.bpb.sectors_per_fat, 9);
  ASSERT_EQ(fat.bpb.sectors_per_track, 18);
  ASSERT_EQ(fat.bpb.num_heads, 2);
}

TEST(test_format_write_read_file) {
  vdisk_t disk;
  vdisk_init(&disk);

  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };

  fat12_err_t err = fat12_format(io, "TEST", false);
  ASSERT_EQ(err, FAT12_OK);

  fat12_t fat;
  err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_OK);

  fat12_writer_t writer;
  err = fat12_open_write(&fat, "HELLO.TXT", &writer);
  ASSERT_EQ(err, FAT12_OK);

  const char *content = "Hello from formatted disk!";
  int written = fat12_write(&writer, (const uint8_t *)content, strlen(content));
  ASSERT_EQ(written, (int)strlen(content));

  err = fat12_close_write(&writer);
  ASSERT_EQ(err, FAT12_OK);

  err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_OK);

  fat12_dirent_t entry;
  err = fat12_find(&fat, "HELLO.TXT", &entry);
  ASSERT_EQ(err, FAT12_OK);
  ASSERT_EQ(entry.size, strlen(content));

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);

  char buf[64];
  int n = fat12_read(&file, (uint8_t *)buf, sizeof(buf));
  ASSERT_EQ(n, (int)strlen(content));
  buf[n] = '\0';
  ASSERT(strcmp(buf, content) == 0);
}

TEST(test_multiple_small_writes) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  fat12_err_t err = fat12_open_write(&fat, "SMALL.TXT", &writer);
  ASSERT_EQ(err, FAT12_OK);

  const char *lines[] = { "a\n", "b\n", "c\n", "d\n", "e\n", "f\n" };
  for (int i = 0; i < 6; i++) {
    int n = fat12_write(&writer, (const uint8_t *)lines[i], 2);
    ASSERT_EQ(n, 2);
  }

  err = fat12_close_write(&writer);
  ASSERT_EQ(err, FAT12_OK);

  fat12_dirent_t entry;
  err = fat12_find(&fat, "SMALL.TXT", &entry);
  ASSERT_EQ(err, FAT12_OK);
  ASSERT_EQ(entry.size, 12);

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);

  char buf[64] = {0};
  int n = fat12_read(&file, (uint8_t *)buf, sizeof(buf));
  ASSERT_EQ(n, 12);
  ASSERT_MEM_EQ(buf, "a\nb\nc\nd\ne\nf\n", 12);
}

TEST(test_single_byte_writes) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  fat12_err_t err = fat12_open_write(&fat, "BYTE.BIN", &writer);
  ASSERT_EQ(err, FAT12_OK);

  for (int i = 0; i < 256; i++) {
    uint8_t b = (uint8_t)i;
    int n = fat12_write(&writer, &b, 1);
    ASSERT_EQ(n, 1);
  }

  err = fat12_close_write(&writer);
  ASSERT_EQ(err, FAT12_OK);

  fat12_dirent_t entry;
  fat12_find(&fat, "BYTE.BIN", &entry);
  ASSERT_EQ(entry.size, 256);

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);

  uint8_t buf[256];
  int n = fat12_read(&file, buf, sizeof(buf));
  ASSERT_EQ(n, 256);

  for (int i = 0; i < 256; i++) {
    if (buf[i] != (uint8_t)i) {
      printf("FAIL\n  Byte %d: expected %02X, got %02X\n", i, (uint8_t)i, buf[i]);
      exit(1);
    }
  }
}

TEST(test_write_exact_cluster_boundary) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  fat12_err_t err = fat12_open_write(&fat, "BOUND.BIN", &writer);
  ASSERT_EQ(err, FAT12_OK);

  uint8_t buf[128];
  for (int chunk = 0; chunk < 8; chunk++) {
    for (int i = 0; i < 128; i++)
      buf[i] = (uint8_t)(chunk * 128 + i);
    int n = fat12_write(&writer, buf, 128);
    ASSERT_EQ(n, 128);
  }

  err = fat12_close_write(&writer);
  ASSERT_EQ(err, FAT12_OK);

  fat12_dirent_t entry;
  fat12_find(&fat, "BOUND.BIN", &entry);
  ASSERT_EQ(entry.size, 1024);

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);

  uint8_t readbuf[1024];
  int n = fat12_read(&file, readbuf, sizeof(readbuf));
  ASSERT_EQ(n, 1024);

  for (int i = 0; i < 1024; i++) {
    if (readbuf[i] != (uint8_t)i) {
      printf("FAIL\n  Byte %d: expected %02X, got %02X\n", i, (uint8_t)i, readbuf[i]);
      exit(1);
    }
  }
}

TEST(test_many_small_writes_large_file) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  fat12_err_t err = fat12_open_write(&fat, "BIG.BIN", &writer);
  ASSERT_EQ(err, FAT12_OK);

  uint32_t total = 10000;
  uint32_t written = 0;
  uint8_t chunk[121];
  while (written < total) {
    uint16_t len = total - written;
    if (len > 121) len = 121;
    for (uint16_t i = 0; i < len; i++)
      chunk[i] = (uint8_t)(written + i);
    int n = fat12_write(&writer, chunk, len);
    ASSERT_EQ(n, (int)len);
    written += len;
  }

  err = fat12_close_write(&writer);
  ASSERT_EQ(err, FAT12_OK);

  fat12_dirent_t entry;
  fat12_find(&fat, "BIG.BIN", &entry);
  ASSERT_EQ(entry.size, total);

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);

  uint8_t buf[512];
  uint32_t verified = 0;
  while (verified < total) {
    uint16_t want = total - verified;
    if (want > 512) want = 512;
    int n = fat12_read(&file, buf, want);
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
}

TEST(test_multiple_small_writes_cross_cluster) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t writer;
  fat12_err_t err = fat12_open_write(&fat, "CROSS.BIN", &writer);
  ASSERT_EQ(err, FAT12_OK);

  uint8_t chunk[100];
  uint32_t written = 0;
  for (int i = 0; i < 20; i++) {
    for (int j = 0; j < 100; j++)
      chunk[j] = (uint8_t)(written + j);
    int n = fat12_write(&writer, chunk, 100);
    ASSERT_EQ(n, 100);
    written += 100;
  }

  err = fat12_close_write(&writer);
  ASSERT_EQ(err, FAT12_OK);

  fat12_dirent_t entry;
  fat12_find(&fat, "CROSS.BIN", &entry);
  ASSERT_EQ(entry.size, 2000);

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);

  uint8_t buf[2000];
  int n = fat12_read(&file, buf, sizeof(buf));
  ASSERT_EQ(n, 2000);

  for (int i = 0; i < 2000; i++) {
    if (buf[i] != (uint8_t)i) {
      printf("FAIL\n  Byte %d: expected %02X, got %02X\n", i, (uint8_t)i, buf[i]);
      exit(1);
    }
  }
}

TEST(test_init_fails_when_boot_read_fails) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = 0;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  ASSERT_EQ(fat12_init(&fat, io), FAT12_ERR_READ);
}

TEST(test_init_fat_mismatch_read_fails) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = 2;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  ASSERT_EQ(fat12_init(&fat, io), FAT12_OK);
}

TEST(test_seek_propagates_read_error) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  fat12_open_write(&fat, "S.TXT", &w);
  uint8_t buf[2000];
  memset(buf, 0x44, sizeof(buf));
  fat12_write(&w, buf, sizeof(buf));
  fat12_close_write(&w);

  fat12_dirent_t e;
  fat12_find(&fat, "S.TXT", &e);

  fat12_file_t file;
  fat12_open(&fat, &e, &file);

  disk.fail_after_reads = 0;
  fat12_err_t err = fat12_seek(&file, 1500);
  ASSERT_EQ(err, FAT12_ERR_READ);
}

TEST(test_read_propagates_read_error) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  fat12_open_write(&fat, "R.TXT", &w);
  uint8_t buf[2000];
  memset(buf, 0x55, sizeof(buf));
  fat12_write(&w, buf, sizeof(buf));
  fat12_close_write(&w);

  fat12_dirent_t e;
  fat12_find(&fat, "R.TXT", &e);

  fat12_file_t file;
  fat12_open(&fat, &e, &file);

  disk.fail_after_reads = 0;
  uint8_t out[2000];
  int n = fat12_read(&file, out, sizeof(out));
  ASSERT(n < 0);
  ASSERT_EQ(-n, FAT12_ERR_READ);
}

TEST(test_close_write_propagates_write_error) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  fat12_open_write(&fat, "W.TXT", &w);
  uint8_t buf[3000];
  memset(buf, 0x66, sizeof(buf));
  fat12_write(&w, buf, sizeof(buf));

  disk.fail_after_track_writes = 0;
  fat12_err_t err = fat12_close_write(&w);
  ASSERT(err != FAT12_OK);
  ASSERT(!fat.batch.active);
}

TEST(test_delete_propagates_write_error) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  fat12_open_write(&fat, "D.TXT", &w);
  uint8_t buf[100];
  memset(buf, 0x77, sizeof(buf));
  fat12_write(&w, buf, sizeof(buf));
  fat12_close_write(&w);

  disk.fail_after_track_writes = 0;
  fat12_err_t err = fat12_delete(&fat, "D.TXT");
  ASSERT_EQ(err, FAT12_ERR_WRITE);
  ASSERT(!fat.batch.active);
}

TEST(test_create_propagates_write_error) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  disk.fail_after_track_writes = 0;
  fat12_dirent_t e;
  fat12_err_t err = fat12_create(&fat, "C.TXT", &e);
  ASSERT_EQ(err, FAT12_ERR_WRITE);
  ASSERT(!fat.batch.active);
}

TEST(test_format_partial_write_failure) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_track_writes = 2;

  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_err_t err = fat12_format(io, "X", true);
  ASSERT_EQ(err, FAT12_ERR_WRITE);
}

TEST(test_close_write_root_entry_flush_fails) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  fat12_open_write(&fat, "F.TXT", &w);
  uint8_t buf[200];
  memset(buf, 0x99, sizeof(buf));
  fat12_write(&w, buf, sizeof(buf));

  disk.fail_after_track_writes = 1;
  fat12_err_t err = fat12_close_write(&w);
  ASSERT(err != FAT12_OK);
  ASSERT(!fat.batch.active);
}

TEST(test_close_write_replacing_existing_chain_free_fails) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  fat12_open_write(&fat, "REP.TXT", &w);
  uint8_t orig[200];
  memset(orig, 0xAA, sizeof(orig));
  fat12_write(&w, orig, sizeof(orig));
  fat12_close_write(&w);

  fat12_open_write(&fat, "REP.TXT", &w);
  ASSERT(w.replacing_existing);
  uint8_t neu[200];
  memset(neu, 0xBB, sizeof(neu));
  fat12_write(&w, neu, sizeof(neu));

  disk.fail_after_track_writes = 2;
  fat12_err_t err = fat12_close_write(&w);
  ASSERT(err != FAT12_OK);
  ASSERT(!fat.batch.active);
}

TEST(test_write_after_error_returns_error) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  fat12_open_write(&fat, "E.TXT", &w);
  w.error = FAT12_ERR_WRITE;

  uint8_t buf[10];
  int n = fat12_write(&w, buf, sizeof(buf));
  ASSERT(n < 0);
  ASSERT_EQ(-n, FAT12_ERR_WRITE);

  fat12_abort_write(&fat);
}

TEST(test_open_write_count_chain_fails) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  fat12_open_write(&fat, "C.TXT", &w);
  uint8_t buf[2500];
  memset(buf, 0x44, sizeof(buf));
  fat12_write(&w, buf, sizeof(buf));
  fat12_close_write(&w);

  disk.fail_after_reads = 1;
  fat12_err_t err = fat12_open_write(&fat, "C.TXT", &w);
  ASSERT(err != FAT12_OK);
  ASSERT(!fat.batch.active);
  ASSERT_NULL(w.fat);
}

TEST(test_open_write_already_active_batch) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w1, w2;
  ASSERT_EQ(fat12_open_write(&fat, "A.TXT", &w1), FAT12_OK);
  ASSERT(fat.batch.active);

  fat12_err_t err = fat12_open_write(&fat, "B.TXT", &w2);
  ASSERT_EQ(err, FAT12_ERR_INVALID);

  fat12_abort_write(&fat);
}

TEST(test_split_sector_fat_entry) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  for (uint16_t c = 2; c < 343; c++) {
    vdisk_set_fat_entry(&disk, c, c == 342 ? 0xFFF : c + 1);
  }
  fat12_init(&fat, io);

  uint16_t next = 0;
  ASSERT_EQ(fat12_get_entry(&fat, 341, &next), FAT12_OK);
  ASSERT_EQ(next, 342);
}

TEST(test_split_sector_fat_entry_read_fail) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  disk.fail_after_reads = -1;
  fat12_init(&fat, io);

  for (uint16_t c = 2; c < 343; c++) {
    vdisk_set_fat_entry(&disk.disk, c, c == 342 ? 0xFFF : c + 1);
  }
  fat12_init(&fat, io);

  disk.fail_after_reads = 1;
  uint16_t next = 0;
  fat12_err_t err = fat12_get_entry(&fat, 341, &next);
  ASSERT_EQ(err, FAT12_ERR_READ);
}

TEST(test_seek_propagates_split_read_fail) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  fat12_open_write(&fat, "L.BIN", &w);
  uint8_t big[2048];
  memset(big, 0xAA, sizeof(big));
  fat12_write(&w, big, sizeof(big));
  fat12_close_write(&w);

  fat12_dirent_t e;
  fat12_find(&fat, "L.BIN", &e);
  fat12_file_t file;
  fat12_open(&fat, &e, &file);

  disk.fail_after_reads = 0;
  fat12_err_t err = fat12_seek(&file, 1024);
  ASSERT_EQ(err, FAT12_ERR_READ);
}

TEST(test_delete_propagates_free_chain_read_error) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  fat12_open_write(&fat, "CHAIN.BIN", &w);
  uint8_t buf[3000];
  memset(buf, 0x77, sizeof(buf));
  fat12_write(&w, buf, sizeof(buf));
  fat12_close_write(&w);

  disk.fail_after_reads = 2;
  fat12_err_t err = fat12_delete(&fat, "CHAIN.BIN");
  ASSERT_EQ(err, FAT12_ERR_READ);
  ASSERT(!fat.batch.active);
}

TEST(test_set_entry_split_write_fail) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  for (uint16_t c = 2; c < 343; c++) {
    vdisk_set_fat_entry(&disk.disk, c, c == 342 ? 0xFFF : c + 1);
  }
  fat12_init(&fat, io);

  fat12_dirent_t e;
  memset(&e, 0, sizeof(e));
  memcpy(e.name, "TAILFILE", 8);
  memcpy(e.ext, "BIN", 3);
  e.attr = FAT12_ATTR_ARCHIVE;
  e.start_cluster = 341;
  e.size = 512;
  uint16_t lba = fat.root_dir_start_sector;
  memcpy(&disk.disk.data[lba][0], &e, sizeof(e));

  disk.fail_after_reads = 1;
  fat12_err_t err = fat12_delete(&fat, "TAILFILE.BIN");
  ASSERT(err != FAT12_OK);
}

TEST(test_format_long_volume_label) {
  vdisk_t disk;
  vdisk_init(&disk);

  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  ASSERT_EQ(fat12_format(io, "LONGLABELSXY", false), FAT12_OK);

  fat12_t fat;
  ASSERT_EQ(fat12_init(&fat, io), FAT12_OK);

  fat12_dirent_t e;
  ASSERT_EQ(fat12_read_root_entry(&fat, 0, &e), FAT12_OK);
  ASSERT(e.attr & FAT12_ATTR_VOLUME_ID);
  ASSERT_MEM_EQ(e.name, "LONGLABE", 8);
  ASSERT_MEM_EQ(e.ext, "LSX", 3);
}

TEST(test_overwrite_in_place_with_data_writes) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  fat12_open_write(&fat, "P.BIN", &w);
  uint8_t orig[2000];
  memset(orig, 0xAA, sizeof(orig));
  fat12_write(&w, orig, sizeof(orig));
  fat12_close_write(&w);

  for (uint16_t cluster = 2; cluster < fat.total_clusters + 2; cluster++) {
    uint16_t next;
    fat12_get_entry(&fat, cluster, &next);
    if (next == 0) {
      vdisk_set_fat_entry(&disk, cluster, 0xFF7);
    }
  }
  fat12_init(&fat, io);

  fat12_open_write(&fat, "P.BIN", &w);
  ASSERT(w.overwrite_in_place);

  uint8_t shorter[600];
  memset(shorter, 0xBB, sizeof(shorter));
  fat12_write(&w, shorter, sizeof(shorter));
  ASSERT_EQ(fat12_close_write(&w), FAT12_OK);

  fat12_dirent_t e;
  fat12_find(&fat, "P.BIN", &e);
  ASSERT_EQ(e.size, sizeof(shorter));
  ASSERT(e.start_cluster >= 2);
}

TEST(test_seek_walks_multiple_clusters) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  fat12_open_write(&fat, "MULTI.BIN", &w);
  uint8_t buf[3000];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i & 0xFF);
  fat12_write(&w, buf, sizeof(buf));
  fat12_close_write(&w);

  fat12_dirent_t e;
  fat12_find(&fat, "MULTI.BIN", &e);
  fat12_file_t file;
  fat12_open(&fat, &e, &file);

  ASSERT_EQ(fat12_seek(&file, 1800), FAT12_OK);
  ASSERT_EQ(file.bytes_read, 1800);

  uint8_t out[10];
  int n = fat12_read(&file, out, sizeof(out));
  ASSERT_EQ(n, 10);
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(out[i], (uint8_t)((1800 + i) & 0xFF));
  }
}

TEST(test_seek_clamps_past_eof_in_fat12) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  ASSERT_EQ(fat12_open_write(&fat, "S.TXT", &w), FAT12_OK);
  uint8_t data[100];
  memset(data, 0xCC, sizeof(data));
  fat12_write(&w, data, sizeof(data));
  fat12_close_write(&w);

  fat12_dirent_t e;
  ASSERT_EQ(fat12_find(&fat, "S.TXT", &e), FAT12_OK);

  fat12_file_t file;
  fat12_open(&fat, &e, &file);
  ASSERT_EQ(fat12_seek(&file, 99999), FAT12_OK);
  ASSERT_EQ(file.bytes_read, sizeof(data));

  uint8_t buf[16];
  ASSERT_EQ(fat12_read(&file, buf, sizeof(buf)), 0);
}

TEST(test_create_existing_returns_exists) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_dirent_t e;
  ASSERT_EQ(fat12_create(&fat, "DUP.TXT", &e), FAT12_OK);
  ASSERT_EQ(fat12_create(&fat, "DUP.TXT", &e), FAT12_ERR_EXISTS);
}

static void write_file(fat12_t *fat, const char *name, const uint8_t *data, size_t len) {
  fat12_writer_t w;
  ASSERT_EQ(fat12_open_write(fat, name, &w), FAT12_OK);
  ASSERT_EQ(fat12_write(&w, data, len), (int)len);
  ASSERT_EQ(fat12_close_write(&w), FAT12_OK);
}

TEST(test_rename_basic) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  uint8_t data[1234];
  for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 7);
  write_file(&fat, "OLD.TXT", data, sizeof(data));

  fat12_dirent_t before;
  ASSERT_EQ(fat12_find(&fat, "OLD.TXT", &before), FAT12_OK);

  ASSERT_EQ(fat12_rename(&fat, "OLD.TXT", "NEW.TXT"), FAT12_OK);

  fat12_dirent_t e;
  ASSERT_EQ(fat12_find(&fat, "OLD.TXT", &e), FAT12_ERR_NOT_FOUND);
  ASSERT_EQ(fat12_find(&fat, "NEW.TXT", &e), FAT12_OK);
  ASSERT_EQ(e.start_cluster, before.start_cluster);
  ASSERT_EQ(e.size, sizeof(data));

  fat12_file_t file;
  fat12_open(&fat, &e, &file);
  uint8_t buf[sizeof(data)];
  ASSERT_EQ(fat12_read(&file, buf, sizeof(buf)), (int)sizeof(buf));
  ASSERT(memcmp(buf, data, sizeof(data)) == 0);
}

TEST(test_rename_missing_source) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  ASSERT_EQ(fat12_rename(&fat, "NOPE.TXT", "NEW.TXT"), FAT12_ERR_NOT_FOUND);
}

TEST(test_rename_to_existing_returns_exists) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  uint8_t data[64] = {1};
  write_file(&fat, "A.TXT", data, sizeof(data));
  write_file(&fat, "B.TXT", data, sizeof(data));

  ASSERT_EQ(fat12_rename(&fat, "A.TXT", "B.TXT"), FAT12_ERR_EXISTS);

  fat12_dirent_t e;
  ASSERT_EQ(fat12_find(&fat, "A.TXT", &e), FAT12_OK);
  ASSERT_EQ(fat12_find(&fat, "B.TXT", &e), FAT12_OK);
}

TEST(test_rename_invalid_names) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  ASSERT_EQ(fat12_rename(&fat, "TOOLONGNAME.TXT", "B.TXT"), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_rename(&fat, "A.TXT", "TOOLONGNAME.TXT"), FAT12_ERR_INVALID);
}

TEST(test_rename_survives_remount) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  uint8_t data[700] = {0x5A};
  write_file(&fat, "KEEP.BIN", data, sizeof(data));
  ASSERT_EQ(fat12_rename(&fat, "KEEP.BIN", "HELD.BIN"), FAT12_OK);

  fat12_t fat2;
  ASSERT_EQ(fat12_init(&fat2, io), FAT12_OK);
  fat12_dirent_t e;
  ASSERT_EQ(fat12_find(&fat2, "HELD.BIN", &e), FAT12_OK);
  ASSERT_EQ(e.size, sizeof(data));
  ASSERT_EQ(fat12_find(&fat2, "KEEP.BIN", &e), FAT12_ERR_NOT_FOUND);
}

TEST(test_free_count_matches_entry_walk) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  uint16_t free0 = 0;
  ASSERT_EQ(fat12_free_count(&fat, &free0), FAT12_OK);
  ASSERT_EQ(free0, count_free_clusters(&fat));
  ASSERT_EQ(free0, fat.total_clusters);

  uint8_t data[3000];
  memset(data, 0xCC, sizeof(data));
  write_file(&fat, "F1.BIN", data, sizeof(data));
  write_file(&fat, "F2.BIN", data, 999);

  uint16_t free1 = 0;
  ASSERT_EQ(fat12_free_count(&fat, &free1), FAT12_OK);
  ASSERT_EQ(free1, count_free_clusters(&fat));
  ASSERT_EQ(free1, free0 - 6 - 2);

  ASSERT_EQ(fat12_delete(&fat, "F1.BIN"), FAT12_OK);
  uint16_t free2 = 0;
  ASSERT_EQ(fat12_free_count(&fat, &free2), FAT12_OK);
  ASSERT_EQ(free2, free1 + 6);
}

TEST(test_failed_write_reclaims_clusters) {
  fault_vdisk_t disk;
  memset(&disk, 0, sizeof(disk));
  vdisk_format_valid(&disk.disk);
  disk.fail_after_reads = -1;
  disk.fail_after_track_writes = -1;

  fat12_t fat;
  fat12_io_t io = { .read = fault_vdisk_read, .write = fault_vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  uint16_t free_before = 0;
  ASSERT_EQ(fat12_free_count(&fat, &free_before), FAT12_OK);

  static uint8_t data[40000];
  memset(data, 0xEE, sizeof(data));

  fat12_writer_t w;
  ASSERT_EQ(fat12_open_write(&fat, "LEAK.BIN", &w), FAT12_OK);
  disk.fail_after_track_writes = 2;
  ASSERT(fat12_write(&w, data, sizeof(data)) < 0);
  disk.fail_after_track_writes = -1;
  ASSERT(fat12_close_write(&w) != FAT12_OK);
  ASSERT(!fat.batch.active);

  fat12_dirent_t e;
  ASSERT_EQ(fat12_find(&fat, "LEAK.BIN", &e), FAT12_ERR_NOT_FOUND);

  uint16_t free_after = 0;
  ASSERT_EQ(fat12_free_count(&fat, &free_after), FAT12_OK);
  ASSERT_EQ(free_after, free_before);
}

TEST(test_volume_label_protected) {
  vdisk_t disk;
  vdisk_init(&disk);

  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  ASSERT_EQ(fat12_format(io, "MYVOL", false), FAT12_OK);

  fat12_t fat;
  ASSERT_EQ(fat12_init(&fat, io), FAT12_OK);

  fat12_dirent_t label;
  ASSERT_EQ(fat12_read_root_entry(&fat, 0, &label), FAT12_OK);
  ASSERT_EQ(label.attr, FAT12_ATTR_VOLUME_ID);

  ASSERT_EQ(fat12_delete(&fat, "MYVOL"), FAT12_ERR_NOT_FOUND);
  ASSERT_EQ(fat12_rename(&fat, "MYVOL", "OTHER"), FAT12_ERR_NOT_FOUND);

  uint8_t data[100];
  memset(data, 0x11, sizeof(data));
  write_file(&fat, "MYVOL", data, sizeof(data));

  ASSERT_EQ(fat12_read_root_entry(&fat, 0, &label), FAT12_OK);
  ASSERT_EQ(label.attr, FAT12_ATTR_VOLUME_ID);
  ASSERT_EQ(label.start_cluster, 0);
  ASSERT_EQ(label.size, 0);

  fat12_dirent_t e;
  bool found_file = false;
  for (uint16_t i = 1; i < fat.bpb.root_entries; i++) {
    if (fat12_read_root_entry(&fat, i, &e) != FAT12_OK) break;
    if (fat12_entry_is_end(&e)) break;
    if (!fat12_entry_valid(&e)) continue;
    if (memcmp(e.name, "MYVOL   ", 8) == 0 && e.attr == FAT12_ATTR_ARCHIVE) {
      ASSERT_EQ(e.size, sizeof(data));
      found_file = true;
    }
  }
  ASSERT(found_file);
}

TEST(test_overwrite_existing_to_empty_in_place) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  ASSERT_EQ(fat12_open_write(&fat, "Z.TXT", &w), FAT12_OK);
  uint8_t big[1500];
  memset(big, 0xAB, sizeof(big));
  fat12_write(&w, big, sizeof(big));
  fat12_close_write(&w);

  fat12_dirent_t e0;
  ASSERT_EQ(fat12_find(&fat, "Z.TXT", &e0), FAT12_OK);
  uint16_t old_start = e0.start_cluster;
  ASSERT(old_start >= 2);

  for (uint16_t cluster = 2; cluster < fat.total_clusters + 2; cluster++) {
    uint16_t next;
    fat12_get_entry(&fat, cluster, &next);
    if (next == 0) {
      vdisk_set_fat_entry(&disk, cluster, 0xFF7);
    }
  }

  fat12_init(&fat, io);

  ASSERT_EQ(fat12_open_write(&fat, "Z.TXT", &w), FAT12_OK);
  ASSERT(w.overwrite_in_place);
  ASSERT_EQ(fat12_close_write(&w), FAT12_OK);

  fat12_dirent_t e;
  ASSERT_EQ(fat12_find(&fat, "Z.TXT", &e), FAT12_OK);
  ASSERT_EQ(e.size, 0);
  ASSERT_EQ(e.start_cluster, 0);

  uint16_t entry;
  fat12_get_entry(&fat, old_start, &entry);
  ASSERT_EQ(entry, 0);
}

TEST(test_writer_unknown_filename_full_dir) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  for (uint16_t i = 0; i < fat.bpb.root_entries; i++) {
    fat12_dirent_t e;
    memset(&e, 0, sizeof(e));
    char name[9];
    snprintf(name, sizeof(name), "F%07u", i + 1);
    memcpy(e.name, name, 8);
    memcpy(e.ext, "TXT", 3);
    e.attr = FAT12_ATTR_ARCHIVE;
    e.start_cluster = 0;
    e.size = 0;
    uint16_t lba = fat.root_dir_start_sector + (i * FAT12_DIR_ENTRY_SIZE) / SECTOR_SIZE;
    uint16_t off = (i * FAT12_DIR_ENTRY_SIZE) % SECTOR_SIZE;
    memcpy(&disk.data[lba][off], &e, sizeof(e));
  }

  fat12_writer_t w;
  fat12_err_t r = fat12_open_write(&fat, "NEWFILE.TXT", &w);
  ASSERT_EQ(r, FAT12_ERR_FULL);
  ASSERT(!fat.batch.active);
}

TEST(test_invalid_filename_in_open_write) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  fat12_writer_t w;
  fat12_err_t r = fat12_open_write(&fat, ".STARTDOT", &w);
  ASSERT_EQ(r, FAT12_ERR_INVALID);
  ASSERT(!fat.batch.active);

  r = fat12_open_write(&fat, "TOOLONGNAME.TXT", &w);
  ASSERT_EQ(r, FAT12_ERR_INVALID);

  r = fat12_open_write(&fat, "BAD/CHAR.TXT", &w);
  ASSERT_EQ(r, FAT12_ERR_INVALID);
}

TEST(test_delete_invalid_filename) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  ASSERT_EQ(fat12_delete(&fat, "BAD/NAME.TXT"), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_delete(&fat, ""), FAT12_ERR_INVALID);
  ASSERT(!fat.batch.active);
}

TEST(test_format_null_write_callback) {
  fat12_io_t io = { .read = vdisk_read, .write = NULL, .ctx = NULL };

  fat12_err_t err = fat12_format(io, "TEST", false);
  ASSERT_EQ(err, FAT12_ERR_INVALID);
}

int main(void) {
  printf("=== FAT12 Tests ===\n\n");

  RUN_TEST(test_init);
  RUN_TEST(test_empty_directory);
  RUN_TEST(test_create_file);
  RUN_TEST(test_create_propagates_find_error);
  RUN_TEST(test_open_write_failure_cleans_up_batch);
  RUN_TEST(test_write_small_file);
  RUN_TEST(test_small_reads_use_cluster_buffer);
  RUN_TEST(test_write_large_file);
  RUN_TEST(test_write_read_large_single_call);
  RUN_TEST(test_overwrite_file);
  RUN_TEST(test_overwrite_failure_preserves_existing_file);
  RUN_TEST(test_overwrite_succeeds_without_spare_clusters);
  RUN_TEST(test_delete_file);
  RUN_TEST(test_delete_marks_dirent_before_freeing_chain);
  RUN_TEST(test_multiple_files);
  RUN_TEST(test_case_insensitive);
  RUN_TEST(test_invalid_83_names_rejected);
  RUN_TEST(test_batching_efficiency);
  RUN_TEST(test_write_read_cycle);
  RUN_TEST(test_cluster_chain);
  RUN_TEST(test_reuse_deleted_entry);
  RUN_TEST(test_fat_entry_manipulation);

  printf("\n--- Incremental Write Tests ---\n");
  RUN_TEST(test_multiple_small_writes);
  RUN_TEST(test_single_byte_writes);
  RUN_TEST(test_write_exact_cluster_boundary);
  RUN_TEST(test_many_small_writes_large_file);
  RUN_TEST(test_multiple_small_writes_cross_cluster);

  printf("\n--- Format Tests ---\n");
  RUN_TEST(test_format_quick);
  RUN_TEST(test_format_full);
  RUN_TEST(test_format_progress_callback);
  RUN_TEST(test_format_no_label);
  RUN_TEST(test_format_then_init);
  RUN_TEST(test_format_write_read_file);
  RUN_TEST(test_format_null_write_callback);

  printf("\n--- Edge Cases ---\n");
  RUN_TEST(test_init_fails_when_boot_read_fails);
  RUN_TEST(test_init_fat_mismatch_read_fails);
  RUN_TEST(test_seek_propagates_read_error);
  RUN_TEST(test_read_propagates_read_error);
  RUN_TEST(test_close_write_propagates_write_error);
  RUN_TEST(test_delete_propagates_write_error);
  RUN_TEST(test_create_propagates_write_error);
  RUN_TEST(test_format_partial_write_failure);
  RUN_TEST(test_close_write_root_entry_flush_fails);
  RUN_TEST(test_close_write_replacing_existing_chain_free_fails);
  RUN_TEST(test_write_after_error_returns_error);
  RUN_TEST(test_open_write_count_chain_fails);
  RUN_TEST(test_open_write_already_active_batch);
  RUN_TEST(test_split_sector_fat_entry);
  RUN_TEST(test_split_sector_fat_entry_read_fail);
  RUN_TEST(test_seek_propagates_split_read_fail);
  RUN_TEST(test_delete_propagates_free_chain_read_error);
  RUN_TEST(test_set_entry_split_write_fail);
  RUN_TEST(test_format_long_volume_label);
  RUN_TEST(test_overwrite_in_place_with_data_writes);
  RUN_TEST(test_seek_walks_multiple_clusters);
  RUN_TEST(test_seek_clamps_past_eof_in_fat12);
  RUN_TEST(test_create_existing_returns_exists);
  RUN_TEST(test_overwrite_existing_to_empty_in_place);
  RUN_TEST(test_rename_basic);
  RUN_TEST(test_rename_missing_source);
  RUN_TEST(test_rename_to_existing_returns_exists);
  RUN_TEST(test_rename_invalid_names);
  RUN_TEST(test_rename_survives_remount);
  RUN_TEST(test_free_count_matches_entry_walk);
  RUN_TEST(test_failed_write_reclaims_clusters);
  RUN_TEST(test_volume_label_protected);
  RUN_TEST(test_writer_unknown_filename_full_dir);
  RUN_TEST(test_invalid_filename_in_open_write);
  RUN_TEST(test_delete_invalid_filename);

  TEST_RESULTS();
}
