#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../src/floppy.h"
#include "../src/fat12.h"

// ============== Virtual Disk ==============
// Simulates a 1.44MB floppy: 80 tracks, 2 sides, 18 sectors/track

#define VDISK_TRACKS 80
#define VDISK_SIDES 2
#define VDISK_TOTAL_SECTORS (VDISK_TRACKS * VDISK_SIDES * SECTORS_PER_TRACK)

typedef struct {
  uint8_t data[VDISK_TOTAL_SECTORS][SECTOR_SIZE];
  int read_count;
  int write_count;
  int track_writes;  // number of track write operations
} vdisk_t;

static inline int vdisk_lba(uint8_t track, uint8_t side, uint8_t sector_n) {
  return (track * VDISK_SIDES + side) * SECTORS_PER_TRACK + (sector_n - 1);
}

bool vdisk_read(void *ctx, sector_t *sector) {
  vdisk_t *disk = (vdisk_t *)ctx;
  int lba = vdisk_lba(sector->track, sector->side, sector->sector_n);

  if (lba < 0 || lba >= VDISK_TOTAL_SECTORS) {
    sector->valid = false;
    return false;
  }

  memcpy(sector->data, disk->data[lba], SECTOR_SIZE);
  sector->valid = true;
  sector->size = SECTOR_SIZE;
  sector->size_code = 2;
  disk->read_count++;
  return true;
}

bool vdisk_write(void *ctx, track_t *track) {
  vdisk_t *disk = (vdisk_t *)ctx;

  // First, read missing sectors (simulating floppy_complete_track)
  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    if (!track->sectors[i].valid) {
      int lba = vdisk_lba(track->track, track->side, i + 1);
      if (lba >= 0 && lba < VDISK_TOTAL_SECTORS) {
        memcpy(track->sectors[i].data, disk->data[lba], SECTOR_SIZE);
        track->sectors[i].valid = true;
        track->sectors[i].track = track->track;
        track->sectors[i].side = track->side;
        track->sectors[i].sector_n = i + 1;
        track->sectors[i].size = SECTOR_SIZE;
        track->sectors[i].size_code = 2;
        disk->read_count++;
      }
    }
  }

  // Now write all sectors
  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    int lba = vdisk_lba(track->track, track->side, i + 1);
    if (lba >= 0 && lba < VDISK_TOTAL_SECTORS) {
      memcpy(disk->data[lba], track->sectors[i].data, SECTOR_SIZE);
    }
  }

  disk->write_count += SECTORS_PER_TRACK;
  disk->track_writes++;
  return true;
}

void vdisk_init(vdisk_t *disk) {
  memset(disk, 0, sizeof(*disk));
}

// ============== Format disk with FAT12 ==============
void vdisk_format(vdisk_t *disk) {
  vdisk_init(disk);

  // Boot sector (LBA 0)
  uint8_t *boot = disk->data[0];

  // Jump instruction
  boot[0] = 0xEB; boot[1] = 0x3C; boot[2] = 0x90;

  // OEM name
  memcpy(&boot[3], "MSDOS5.0", 8);

  // BPB
  boot[11] = SECTOR_SIZE & 0xFF;         // bytes per sector low
  boot[12] = SECTOR_SIZE >> 8;           // bytes per sector high
  boot[13] = 1;                          // sectors per cluster
  boot[14] = 1; boot[15] = 0;            // reserved sectors (1)
  boot[16] = 2;                          // number of FATs
  boot[17] = 224; boot[18] = 0;          // root entries (224)
  boot[19] = (VDISK_TOTAL_SECTORS) & 0xFF;  // total sectors low
  boot[20] = (VDISK_TOTAL_SECTORS) >> 8;    // total sectors high
  boot[21] = 0xF0;                       // media descriptor (1.44MB floppy)
  boot[22] = 9; boot[23] = 0;            // sectors per FAT (9)
  boot[24] = 18; boot[25] = 0;           // sectors per track (18)
  boot[26] = 2; boot[27] = 0;            // number of heads (2)
  boot[28] = 0; boot[29] = 0; boot[30] = 0; boot[31] = 0;  // hidden sectors

  // Boot signature
  boot[510] = 0x55;
  boot[511] = 0xAA;

  // FAT1 (LBA 1-9) and FAT2 (LBA 10-18)
  // First two entries are reserved
  uint8_t *fat1 = disk->data[1];
  uint8_t *fat2 = disk->data[10];

  fat1[0] = 0xF0;  // media descriptor
  fat1[1] = 0xFF;
  fat1[2] = 0xFF;

  fat2[0] = 0xF0;
  fat2[1] = 0xFF;
  fat2[2] = 0xFF;

  // Root directory starts at LBA 19 (1 + 9 + 9)
  // 224 entries * 32 bytes = 7168 bytes = 14 sectors
  // Data area starts at LBA 33
}

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

#define ASSERT_STR_EQ(a, b, len) do { \
  if (memcmp(a, b, len) != 0) { \
    printf("FAIL\n  String mismatch at %s:%d\n", __FILE__, __LINE__); \
    exit(1); \
  } \
} while(0)

// ============== Tests ==============

TEST(test_init) {
  vdisk_t disk;
  vdisk_format(&disk);

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
  vdisk_format(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  // Try to find a file that doesn't exist
  fat12_dirent_t entry;
  fat12_err_t err = fat12_find(&fat, "NOFILE.TXT", &entry);
  ASSERT_EQ(err, FAT12_ERR_NOT_FOUND);
}

TEST(test_create_file) {
  vdisk_t disk;
  vdisk_format(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  // Create a new file
  fat12_dirent_t entry;
  fat12_err_t err = fat12_create(&fat, "TEST.TXT", &entry);
  ASSERT_EQ(err, FAT12_OK);

  // Should be able to find it now
  fat12_dirent_t found;
  err = fat12_find(&fat, "TEST.TXT", &found);
  ASSERT_EQ(err, FAT12_OK);
  ASSERT_STR_EQ(found.name, "TEST    ", 8);
  ASSERT_STR_EQ(found.ext, "TXT", 3);
  ASSERT_EQ(found.size, 0);
}

TEST(test_write_small_file) {
  vdisk_t disk;
  vdisk_format(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  // Open file for writing
  fat12_writer_t writer;
  fat12_err_t err = fat12_open_write(&fat, "HELLO.TXT", &writer);
  ASSERT_EQ(err, FAT12_OK);

  // Write some data
  const char *msg = "Hello, World!";
  int written = fat12_write(&writer, (uint8_t *)msg, strlen(msg));
  ASSERT_EQ(written, (int)strlen(msg));

  // Close file
  err = fat12_close_write(&writer);
  ASSERT_EQ(err, FAT12_OK);

  // Read it back
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
  ASSERT_STR_EQ(buf, msg, strlen(msg));
}

TEST(test_write_large_file) {
  vdisk_t disk;
  vdisk_format(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  // Write a file larger than one cluster (512 bytes)
  fat12_writer_t writer;
  fat12_err_t err = fat12_open_write(&fat, "BIG.DAT", &writer);
  ASSERT_EQ(err, FAT12_OK);

  // Write 2000 bytes of pattern data
  uint8_t pattern[2000];
  for (int i = 0; i < 2000; i++) {
    pattern[i] = i & 0xFF;
  }

  int written = fat12_write(&writer, pattern, sizeof(pattern));
  ASSERT_EQ(written, 2000);

  err = fat12_close_write(&writer);
  ASSERT_EQ(err, FAT12_OK);

  // Read it back
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
  vdisk_format(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  // Write initial file
  fat12_writer_t writer;
  fat12_open_write(&fat, "DATA.TXT", &writer);
  fat12_write(&writer, (uint8_t *)"First version", 13);
  fat12_close_write(&writer);

  // Overwrite with new content
  fat12_open_write(&fat, "DATA.TXT", &writer);
  fat12_write(&writer, (uint8_t *)"Second", 6);
  fat12_close_write(&writer);

  // Read back
  fat12_dirent_t entry;
  fat12_find(&fat, "DATA.TXT", &entry);
  ASSERT_EQ(entry.size, 6);

  fat12_file_t file;
  fat12_open(&fat, &entry, &file);

  char buf[64] = {0};
  fat12_read(&file, (uint8_t *)buf, sizeof(buf));
  ASSERT_STR_EQ(buf, "Second", 6);
}

TEST(test_delete_file) {
  vdisk_t disk;
  vdisk_format(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  // Create and write a file
  fat12_writer_t writer;
  fat12_open_write(&fat, "DELETE.ME", &writer);
  fat12_write(&writer, (uint8_t *)"To be deleted", 13);
  fat12_close_write(&writer);

  // Verify it exists
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "DELETE.ME", &entry), FAT12_OK);

  // Delete it
  fat12_err_t err = fat12_delete(&fat, "DELETE.ME");
  ASSERT_EQ(err, FAT12_OK);

  // Should not be found anymore
  err = fat12_find(&fat, "DELETE.ME", &entry);
  ASSERT_EQ(err, FAT12_ERR_NOT_FOUND);
}

TEST(test_multiple_files) {
  vdisk_t disk;
  vdisk_format(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  // Create several files
  const char *names[] = {"FILE1.TXT", "FILE2.TXT", "FILE3.TXT", "DATA.BIN"};
  const char *contents[] = {"Content 1", "Content 2", "Content 3", "Binary"};

  for (int i = 0; i < 4; i++) {
    fat12_writer_t writer;
    fat12_open_write(&fat, names[i], &writer);
    fat12_write(&writer, (uint8_t *)contents[i], strlen(contents[i]));
    fat12_close_write(&writer);
  }

  // Read them all back
  for (int i = 0; i < 4; i++) {
    fat12_dirent_t entry;
    fat12_err_t err = fat12_find(&fat, names[i], &entry);
    ASSERT_EQ(err, FAT12_OK);
    ASSERT_EQ(entry.size, strlen(contents[i]));

    fat12_file_t file;
    fat12_open(&fat, &entry, &file);

    char buf[64] = {0};
    fat12_read(&file, (uint8_t *)buf, sizeof(buf));
    ASSERT_STR_EQ(buf, contents[i], strlen(contents[i]));
  }
}

TEST(test_case_insensitive) {
  vdisk_t disk;
  vdisk_format(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  // Create file with uppercase
  fat12_writer_t writer;
  fat12_open_write(&fat, "UPPER.TXT", &writer);
  fat12_write(&writer, (uint8_t *)"test", 4);
  fat12_close_write(&writer);

  // Find with lowercase
  fat12_dirent_t entry;
  fat12_err_t err = fat12_find(&fat, "upper.txt", &entry);
  ASSERT_EQ(err, FAT12_OK);

  // Find with mixed case
  err = fat12_find(&fat, "Upper.Txt", &entry);
  ASSERT_EQ(err, FAT12_OK);
}

TEST(test_batching_efficiency) {
  vdisk_t disk;
  vdisk_format(&disk);
  disk.track_writes = 0;

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  // Write a file that spans multiple sectors on the same track
  // The data area starts at LBA 33, which is track 0, side 1, sector 16
  // Writing ~5KB should use ~10 sectors, mostly on same track
  fat12_writer_t writer;
  fat12_open_write(&fat, "BATCH.DAT", &writer);

  uint8_t data[5000];
  memset(data, 0xAA, sizeof(data));
  fat12_write(&writer, data, sizeof(data));
  fat12_close_write(&writer);

  // Check that we didn't do too many track writes
  // With good batching, we should need only a few track writes
  printf("\n  Track writes: %d (sectors written: %d)\n  ",
         disk.track_writes, disk.write_count);

  // Should be much less than 10 track writes for 10 sectors
  // Allow up to 6 due to FAT updates across tracks
  ASSERT(disk.track_writes <= 6);
}

TEST(test_write_read_cycle) {
  vdisk_t disk;
  vdisk_format(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  // Multiple write/read cycles
  for (int cycle = 0; cycle < 3; cycle++) {
    char filename[16];
    sprintf(filename, "CYCLE%d.DAT", cycle);

    // Write
    fat12_writer_t writer;
    fat12_open_write(&fat, filename, &writer);

    uint8_t data[1024];
    for (int i = 0; i < 1024; i++) {
      data[i] = (cycle * 100 + i) & 0xFF;
    }
    fat12_write(&writer, data, sizeof(data));
    fat12_close_write(&writer);

    // Read back immediately
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
  vdisk_format(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  // Write a file spanning multiple clusters
  fat12_writer_t writer;
  fat12_open_write(&fat, "CHAIN.DAT", &writer);

  // Write 3KB (6 clusters with 512 byte clusters)
  uint8_t data[3072];
  for (int i = 0; i < 3072; i++) {
    data[i] = i & 0xFF;
  }
  fat12_write(&writer, data, sizeof(data));
  fat12_close_write(&writer);

  // Read back and verify cluster chain works
  fat12_dirent_t entry;
  fat12_find(&fat, "CHAIN.DAT", &entry);
  ASSERT_EQ(entry.size, 3072);
  ASSERT(entry.start_cluster >= 2);  // Valid cluster

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
  vdisk_format(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  // Create and delete a file
  fat12_writer_t writer;
  fat12_open_write(&fat, "FIRST.TXT", &writer);
  fat12_write(&writer, (uint8_t *)"First", 5);
  fat12_close_write(&writer);

  fat12_delete(&fat, "FIRST.TXT");

  // Create a new file - should reuse the deleted entry
  fat12_open_write(&fat, "SECOND.TXT", &writer);
  fat12_write(&writer, (uint8_t *)"Second", 6);
  fat12_close_write(&writer);

  // Verify
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "SECOND.TXT", &entry), FAT12_OK);
  ASSERT_EQ(entry.size, 6);
}

TEST(test_fat_entry_manipulation) {
  vdisk_t disk;
  vdisk_format(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  // Write a multi-cluster file
  fat12_writer_t writer;
  fat12_open_write(&fat, "MULTI.DAT", &writer);

  uint8_t data[2048];  // 4 clusters
  memset(data, 0x55, sizeof(data));
  fat12_write(&writer, data, sizeof(data));
  fat12_close_write(&writer);

  // Find the file and trace its cluster chain
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

  // Should have 4 clusters in chain (2048 bytes / 512 bytes per cluster)
  ASSERT_EQ(chain_length, 4);
}

// ============== Format Tests ==============

TEST(test_format_quick) {
  vdisk_t disk;
  memset(&disk, 0xFF, sizeof(disk));  // Fill with 0xFF to detect unwritten sectors
  disk.track_writes = 0;

  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };

  // Quick format
  fat12_err_t err = fat12_format(io, "TESTDISK", false);
  ASSERT_EQ(err, FAT12_OK);

  // Should have written only a few tracks (boot + FATs + root dir)
  ASSERT(disk.track_writes <= 4);

  // Verify boot sector
  uint8_t *boot = disk.data[0];
  ASSERT_EQ(boot[0], 0xEB);  // Jump
  ASSERT_EQ(boot[510], 0x55);
  ASSERT_EQ(boot[511], 0xAA);
  ASSERT_EQ(boot[11] | (boot[12] << 8), 512);  // bytes per sector
  ASSERT_EQ(boot[13], 1);  // sectors per cluster
  ASSERT_EQ(boot[16], 2);  // num FATs
  ASSERT_EQ(boot[21], 0xF0);  // media descriptor

  // Verify FAT
  ASSERT_EQ(disk.data[1][0], 0xF0);  // Media descriptor
  ASSERT_EQ(disk.data[1][1], 0xFF);
  ASSERT_EQ(disk.data[1][2], 0xFF);

  // Second FAT should be a copy
  ASSERT_EQ(disk.data[10][0], 0xF0);
  ASSERT_EQ(disk.data[10][1], 0xFF);
  ASSERT_EQ(disk.data[10][2], 0xFF);

  // Volume label in root directory
  fat12_dirent_t *label = (fat12_dirent_t *)disk.data[19];
  ASSERT(memcmp(label->name, "TESTDISK", 8) == 0);
  ASSERT_EQ(label->attr, FAT12_ATTR_VOLUME_ID);
}

TEST(test_format_full) {
  vdisk_t disk;
  memset(&disk, 0xFF, sizeof(disk));
  disk.track_writes = 0;

  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };

  // Full format
  fat12_err_t err = fat12_format(io, "FULLDISK", true);
  ASSERT_EQ(err, FAT12_OK);

  // Should have written all 160 tracks (80 tracks * 2 sides)
  ASSERT_EQ(disk.track_writes, 160);

  // Verify data area is zeroed
  ASSERT_EQ(disk.data[33][0], 0);  // First data sector
  ASSERT_EQ(disk.data[2879][0], 0);  // Last sector
}

TEST(test_format_no_label) {
  vdisk_t disk;
  vdisk_init(&disk);

  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };

  fat12_err_t err = fat12_format(io, NULL, false);
  ASSERT_EQ(err, FAT12_OK);

  // Boot sector should have "NO NAME" label
  uint8_t *boot = disk.data[0];
  ASSERT(memcmp(&boot[43], "NO NAME    ", 11) == 0);

  // Root directory should be empty (no volume label entry)
  fat12_dirent_t *first_entry = (fat12_dirent_t *)disk.data[19];
  ASSERT_EQ((uint8_t)first_entry->name[0], 0);  // Empty entry
}

TEST(test_format_then_init) {
  vdisk_t disk;
  vdisk_init(&disk);

  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };

  // Format the disk
  fat12_err_t err = fat12_format(io, "MYDISK", false);
  ASSERT_EQ(err, FAT12_OK);

  // Initialize FAT12 from the formatted disk
  fat12_t fat;
  err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_OK);

  // Verify BPB was parsed correctly
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

  // Format
  fat12_err_t err = fat12_format(io, "TEST", false);
  ASSERT_EQ(err, FAT12_OK);

  // Init
  fat12_t fat;
  err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_OK);

  // Write a file
  fat12_writer_t writer;
  err = fat12_open_write(&fat, "HELLO.TXT", &writer);
  ASSERT_EQ(err, FAT12_OK);

  const char *content = "Hello from formatted disk!";
  int written = fat12_write(&writer, (const uint8_t *)content, strlen(content));
  ASSERT_EQ(written, (int)strlen(content));

  err = fat12_close_write(&writer);
  ASSERT_EQ(err, FAT12_OK);

  // Re-init to ensure we read from disk
  err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_OK);

  // Find and read the file
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

  printf("\n--- Format Tests ---\n");
  RUN_TEST(test_format_quick);
  RUN_TEST(test_format_full);
  RUN_TEST(test_format_no_label);
  RUN_TEST(test_format_then_init);
  RUN_TEST(test_format_write_read_file);
  RUN_TEST(test_format_null_write_callback);

  printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);

  return (tests_passed == tests_run) ? 0 : 1;
}
