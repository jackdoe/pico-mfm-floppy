#include "test.h"
#include "vdisk.h"
#include "../src/fat12.h"

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
  err = fat12_open(&fat, &entry, &file);
  ASSERT_EQ(err, FAT12_OK);

  char buf[64] = {0};
  int n = fat12_read(&file, (uint8_t *)buf, sizeof(buf));
  ASSERT_EQ(n, (int)strlen(msg));
  ASSERT_MEM_EQ(buf, msg, strlen(msg));
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
  err = fat12_open(&fat, &entry, &file);
  ASSERT_EQ(err, FAT12_OK);

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
  RUN_TEST(test_write_small_file);
  RUN_TEST(test_write_large_file);
  RUN_TEST(test_overwrite_file);
  RUN_TEST(test_delete_file);
  RUN_TEST(test_multiple_files);
  RUN_TEST(test_case_insensitive);
  RUN_TEST(test_batching_efficiency);
  RUN_TEST(test_write_read_cycle);
  RUN_TEST(test_cluster_chain);
  RUN_TEST(test_reuse_deleted_entry);
  RUN_TEST(test_fat_entry_manipulation);

  printf("\n--- Small Writes Tests ---\n");
  RUN_TEST(test_multiple_small_writes);
  RUN_TEST(test_multiple_small_writes_cross_cluster);

  printf("\n--- Format Tests ---\n");
  RUN_TEST(test_format_quick);
  RUN_TEST(test_format_full);
  RUN_TEST(test_format_no_label);
  RUN_TEST(test_format_then_init);
  RUN_TEST(test_format_write_read_file);
  RUN_TEST(test_format_null_write_callback);

  TEST_RESULTS();
}
