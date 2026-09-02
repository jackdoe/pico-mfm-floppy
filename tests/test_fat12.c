#include "test.h"
#include "vdisk.h"
#include "../examples/fsck_scenario.h"

static vdisk_t disk;
static cache_t cache;
static fat12_t fat;
static uint8_t data_a[65536];
static uint8_t data_b[65536];
static uint8_t read_buffer[65536];

static disk_err_t mount_into(fat12_t *target, vdisk_t *device) {
  ASSERT_EQ(cache_init(&cache, vdisk_device(device)), DISK_OK);
  ASSERT_EQ(cache_bind(&cache), DISK_OK);
  return fat12_init(target, &cache);
}

static void mount(vdisk_t *device) {
  ASSERT_EQ(mount_into(&fat, device), DISK_OK);
}

static void mount_clean(void) {
  vdisk_format_valid(&disk);
  mount(&disk);
}

static void reload(void) {
  cache_clear(&cache);
}

static void fill_pattern(uint8_t *data, size_t length, uint32_t salt) {
  for (size_t index = 0; index < length; index++) {
    data[index] = (uint8_t)(index * 37u + salt * 19u + index / 251u);
  }
}

static uint64_t disk_digest(const vdisk_t *target) {
  uint64_t digest = UINT64_C(1469598103934665603);
  for (size_t lba = 0; lba < DISK_SECTOR_COUNT; lba++) {
    for (size_t offset = 0; offset < DISK_SECTOR_SIZE; offset++) {
      digest ^= target->data[lba][offset];
      digest *= UINT64_C(1099511628211);
    }
  }
  return digest;
}

static void write_file(fat12_t *filesystem, const char *name,
                       const uint8_t *data, size_t length) {
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(filesystem, name, &writer), DISK_OK);
  size_t offset = 0;
  while (offset < length) {
    disk_result_t result = fat12_write(
        &writer, data + offset, length - offset);
    offset += result.count;
    ASSERT_EQ(result.error, DISK_OK);
  }
  ASSERT_EQ(fat12_close_write(&writer), DISK_OK);
}

static size_t read_file(fat12_t *filesystem, const char *name,
                        uint8_t *out, size_t capacity) {
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(filesystem, name, &entry), DISK_OK);
  fat12_file_t file;
  ASSERT_EQ(fat12_open(filesystem, &entry, &file), DISK_OK);
  disk_result_t result = fat12_read(&file, out, capacity);
  ASSERT_EQ(result.error, DISK_OK);
  return result.count;
}

static void raw_store16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

static void raw_store32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

static void raw_dirent_at(vdisk_t *target, uint16_t lba, uint16_t offset,
                          const char name[static 8], const char ext[static 3],
                          uint8_t attr, uint16_t start, uint32_t size) {
  uint8_t *raw = target->data[lba] + offset;
  memset(raw, 0, FAT12_DIR_ENTRY_SIZE);
  memcpy(raw, name, 8);
  memcpy(raw + 8, ext, 3);
  raw[11] = attr;
  raw_store16(raw + 26, start);
  raw_store32(raw + 28, size);
  reload();
}

static void raw_dirent(vdisk_t *target, uint16_t index,
                       const char name[static 8], const char ext[static 3],
                       uint8_t attr, uint16_t start, uint32_t size) {
  uint16_t lba = (uint16_t)(FAT12_ROOT_START +
      index * FAT12_DIR_ENTRY_SIZE / DISK_SECTOR_SIZE);
  uint16_t offset = (uint16_t)(index * FAT12_DIR_ENTRY_SIZE % DISK_SECTOR_SIZE);
  raw_dirent_at(target, lba, offset, name, ext, attr, start, size);
}

static void set_fat(vdisk_t *target, uint16_t cluster, uint16_t value) {
  vdisk_set_fat_entry(target, cluster, value);
  reload();
}

static void set_fat_copy(vdisk_t *target, uint16_t fat_start, uint16_t cluster,
                         uint16_t value) {
  vdisk_set_fat_copy_entry(target, fat_start, cluster, value);
  reload();
}

static uint16_t entry_next(fat12_t *filesystem, uint16_t cluster) {
  uint16_t next = 0;
  ASSERT_EQ(fat12_get_entry(filesystem, cluster, &next), DISK_OK);
  return next;
}

static uint16_t free_clusters(fat12_t *filesystem) {
  uint16_t count = 0;
  ASSERT_EQ(fat12_free_count(filesystem, &count), DISK_OK);
  return count;
}

static fat12_fsck_t check(fat12_t *filesystem) {
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(filesystem, &report, false), DISK_OK);
  return report;
}

static fat12_fsck_t repair(fat12_t *filesystem) {
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(filesystem, &report, true), DISK_OK);
  return report;
}

static void make_dir(uint16_t root_index, const char name[static 8],
                     uint16_t start) {
  raw_dirent(&disk, root_index, name, "   ", FAT12_ATTR_DIRECTORY, start, 0);
  set_fat(&disk, start, 0xFFF);
  memset(disk.data[FAT12_DATA_START + start - 2u], 0, DISK_SECTOR_SIZE);
  reload();
}

static void expect_clean(fat12_t *filesystem) {
  fat12_fsck_t report = check(filesystem);
  ASSERT(fat12_fsck_clean(&report));
}

TEST(test_strict_geometry_and_little_endian) {
  mount_clean();
  ASSERT_EQ(FAT12_DATA_START, 33);
  ASSERT_EQ(FAT12_DATA_CLUSTERS, 2847);

  static const uint8_t fields[] = {
    11, 13, 14, 16, 17, 19, 21, 22, 24, 26, 28, 32
  };
  for (size_t index = 0; index < sizeof(fields); index++) {
    vdisk_format_valid(&disk);
    disk.data[0][fields[index]] ^= 0x5A;
    ASSERT_EQ(mount_into(&fat, &disk), DISK_ERR_INVALID);
  }

  vdisk_format_valid(&disk);
  disk.data[0][11] = 0;
  disk.data[0][12] = 2;
  ASSERT_EQ(mount_into(&fat, &disk), DISK_OK);
}

TEST(test_init_reads_every_fat_copy) {
  vdisk_format_valid(&disk);
  disk.reads_before_failure = 1;
  ASSERT_EQ(mount_into(&fat, &disk), DISK_ERR_TIMEOUT);

  vdisk_format_valid(&disk);
  disk.data[FAT12_FAT2_START + 8][17] ^= 0x80;
  mount(&disk);
  ASSERT(check(&fat).fat_mismatch);
}

TEST(test_canonical_name_api) {
  fat12_name_t name;
  ASSERT_EQ(fat12_name_parse("alpha.bin", &name), DISK_OK);
  ASSERT_MEM_EQ(name.name, "ALPHA   ", 8);
  ASSERT_MEM_EQ(name.ext, "BIN", 3);
  ASSERT_EQ(fat12_name_parse("TOO-LONG-NAME.BIN", &name), DISK_ERR_INVALID);
  ASSERT_EQ(fat12_name_parse("A.B.C", &name), DISK_ERR_INVALID);
  ASSERT_EQ(fat12_name_parse("BAD NAME", &name), DISK_ERR_INVALID);
}

TEST(test_open_is_fallible_and_null_safe) {
  mount_clean();
  fat12_dirent_t entry;
  memset(&entry, 0, sizeof(entry));
  memset(entry.name, ' ', sizeof(entry.name));
  memset(entry.ext, ' ', sizeof(entry.ext));
  entry.attr = FAT12_ATTR_ARCHIVE;
  fat12_file_t file;
  ASSERT_EQ(fat12_open(NULL, &entry, &file), DISK_ERR_INVALID);
  ASSERT_EQ(fat12_open(&fat, NULL, &file), DISK_ERR_INVALID);
  ASSERT_EQ(fat12_open(&fat, &entry, NULL), DISK_ERR_INVALID);
  ASSERT_EQ(fat12_open(&fat, &entry, &file), DISK_OK);
}

TEST(test_create_write_seek_read_delete_rename) {
  mount_clean();
  fill_pattern(data_a, 7000, 1);
  write_file(&fat, "FIRST.BIN", data_a, 7000);
  ASSERT_EQ(read_file(&fat, "FIRST.BIN", read_buffer, sizeof(read_buffer)), 7000);
  ASSERT_MEM_EQ(read_buffer, data_a, 7000);

  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "FIRST.BIN", &entry), DISK_OK);
  fat12_file_t file;
  ASSERT_EQ(fat12_open(&fat, &entry, &file), DISK_OK);
  ASSERT_EQ(fat12_seek(&file, 513), DISK_OK);
  disk_result_t result = fat12_read(&file, read_buffer, 1000);
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, 1000);
  ASSERT_MEM_EQ(read_buffer, data_a + 513, 1000);

  ASSERT_EQ(fat12_rename(&fat, "FIRST.BIN", "SECOND.BIN"), DISK_OK);
  ASSERT_EQ(fat12_find(&fat, "FIRST.BIN", &entry), DISK_ERR_NOT_FOUND);
  ASSERT_EQ(fat12_delete(&fat, "SECOND.BIN"), DISK_OK);
  ASSERT_EQ(fat12_find(&fat, "SECOND.BIN", &entry), DISK_ERR_NOT_FOUND);
  ASSERT_EQ(free_clusters(&fat), FAT12_DATA_CLUSTERS);
  ASSERT(!cache_dirty(&cache));
}

TEST(test_directories_are_not_files) {
  mount_clean();
  set_fat(&disk, 100, 0xFFF);
  raw_dirent(&disk, 0, "SUBDIR  ", "   ", FAT12_ATTR_DIRECTORY, 100, 0);
  memset(disk.data[FAT12_DATA_START + 98], 0, DISK_SECTOR_SIZE);
  reload();
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "SUBDIR", &entry), DISK_OK);
  ASSERT_EQ(entry.attr, FAT12_ATTR_DIRECTORY);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "SUBDIR", &writer), DISK_ERR_IS_DIR);
  ASSERT_EQ(fat12_delete(&fat, "SUBDIR"), DISK_ERR_IS_DIR);
  ASSERT_EQ(fat12_rename(&fat, "SUBDIR", "OTHER"), DISK_ERR_IS_DIR);
  ASSERT_EQ(fat12_rename(&fat, "MISSING", "SUBDIR"), DISK_ERR_EXISTS);
  ASSERT_EQ(disk.track_writes, 0);
}

TEST(test_typed_read_preserves_partial_progress) {
  mount_clean();
  fill_pattern(data_a, 4000, 2);
  write_file(&fat, "READ.BIN", data_a, 4000);
  reload();

  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "READ.BIN", &entry), DISK_OK);
  fat12_file_t file;
  ASSERT_EQ(fat12_open(&fat, &entry, &file), DISK_OK);
  disk.fail_track = 2;
  disk_result_t result = fat12_read(&file, read_buffer, 4000);
  ASSERT_EQ(result.error, DISK_ERR_TIMEOUT);
  ASSERT_EQ(result.count, 1536);
  ASSERT_MEM_EQ(read_buffer, data_a, result.count);

  disk.fail_track = -1;
  result = fat12_read(&file, read_buffer + 1536, 4000 - 1536);
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, 4000 - 1536);
  ASSERT_MEM_EQ(read_buffer, data_a, 4000);
}

TEST(test_sequential_read_reads_each_track_once) {
  mount_clean();
  fill_pattern(data_a, sizeof(data_a), 21);
  int writes = disk.track_writes;
  write_file(&fat, "LINEAR.BIN", data_a, sizeof(data_a));
  ASSERT(disk.track_writes - writes <= 12);
  reload();
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "LINEAR.BIN", &entry), DISK_OK);
  ASSERT_EQ(entry.start_cluster, 2);
  uint16_t first_lba = FAT12_DATA_START;
  uint16_t last_lba = (uint16_t)(first_lba + sizeof(data_a) / DISK_SECTOR_SIZE - 1u);
  int data_tracks = (int)(last_lba / DISK_SECTORS_PER_TRACK -
                          first_lba / DISK_SECTORS_PER_TRACK + 1u);
  int fat_tracks = 1;
  fat12_file_t file;
  disk.track_reads = 0;
  ASSERT_EQ(fat12_open(&fat, &entry, &file), DISK_OK);
  ASSERT(disk.track_reads <= fat_tracks);
  disk_result_t result = fat12_read(&file, read_buffer, sizeof(read_buffer));
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, sizeof(data_a));
  ASSERT(disk.track_reads <= fat_tracks + data_tracks);
  ASSERT_MEM_EQ(read_buffer, data_a, sizeof(data_a));
}

TEST(test_read_rejects_cycles_without_replaying_data) {
  mount_clean();
  set_fat(&disk, 2, 3);
  set_fat(&disk, 3, 2);
  raw_dirent(&disk, 0, "CYCLE   ", "BIN", FAT12_ATTR_ARCHIVE, 2, 1536);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "CYCLE.BIN", &entry), DISK_OK);
  fat12_file_t file;
  ASSERT_EQ(fat12_open(&fat, &entry, &file), DISK_ERR_CORRUPT);
}

TEST(test_typed_write_preserves_partial_progress_and_retries) {
  mount_clean();
  fill_pattern(data_a, 50000, 3);

  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "RETRY.BIN", &writer), DISK_OK);
  disk.writes_before_failure = 0;
  disk_result_t first = fat12_write(&writer, data_a, 50000);
  ASSERT_EQ(first.error, DISK_ERR_VERIFY);
  ASSERT(first.count > 0);
  ASSERT(first.count < 50000);

  disk.writes_before_failure = -1;
  disk_result_t second = fat12_write(
      &writer, data_a + first.count, 50000 - first.count);
  ASSERT_EQ(second.error, DISK_OK);
  ASSERT_EQ(first.count + second.count, 50000);
  ASSERT_EQ(fat12_close_write(&writer), DISK_OK);
  ASSERT_EQ(read_file(&fat, "RETRY.BIN", read_buffer, sizeof(read_buffer)), 50000);
  ASSERT_MEM_EQ(read_buffer, data_a, 50000);
}

TEST(test_close_retries_each_commit_phase) {
  for (int failure = 0; failure < 4; failure++) {
    mount_clean();
    fill_pattern(data_a, 900, (uint32_t)failure + 4u);
    fat12_writer_t writer;
    ASSERT_EQ(fat12_open_write(&fat, "PHASE.BIN", &writer), DISK_OK);
    disk_result_t written = fat12_write(&writer, data_a, 900);
    ASSERT_EQ(written.error, DISK_OK);
    ASSERT_EQ(written.count, 900);

    disk.writes_before_failure = failure;
    disk_err_t result = fat12_close_write(&writer);
    if (result == DISK_OK) continue;
    ASSERT_EQ(result, DISK_ERR_VERIFY);
    ASSERT(fat12_busy(&fat));
    ASSERT_EQ(fat12_abort_write(&writer), DISK_ERR_BUSY);
    disk.writes_before_failure = -1;
    ASSERT_EQ(fat12_close_write(&writer), DISK_OK);
    ASSERT(!fat12_busy(&fat));
    ASSERT_EQ(read_file(&fat, "PHASE.BIN", read_buffer, sizeof(read_buffer)), 900);
    ASSERT_MEM_EQ(read_buffer, data_a, 900);
  }
}

TEST(test_close_retries_reclaim_phase_when_replacing) {
  for (int failure = 0; failure < 16; failure++) {
    mount_clean();
    fat12_writer_t writer;
    ASSERT_EQ(fat12_open_write(&fat, "REPL.BIN", &writer), DISK_OK);
    fill_pattern(data_a, 2000, 1u);
    ASSERT_EQ(fat12_write(&writer, data_a, 2000).error, DISK_OK);
    ASSERT_EQ(fat12_close_write(&writer), DISK_OK);

    ASSERT_EQ(fat12_open_write(&fat, "REPL.BIN", &writer), DISK_OK);
    fill_pattern(data_b, 1000, 2u);
    ASSERT_EQ(fat12_write(&writer, data_b, 1000).error, DISK_OK);
    disk.writes_before_failure = failure;
    disk_err_t result = fat12_close_write(&writer);
    if (result != DISK_OK) {
      ASSERT_EQ(result, DISK_ERR_VERIFY);
      ASSERT(fat12_busy(&fat));
      ASSERT_EQ(fat12_abort_write(&writer), DISK_ERR_BUSY);
      disk.writes_before_failure = -1;
      ASSERT_EQ(fat12_close_write(&writer), DISK_OK);
    }
    ASSERT(!fat12_busy(&fat));
    ASSERT_EQ(read_file(&fat, "REPL.BIN", read_buffer, sizeof(read_buffer)), 1000);
    ASSERT_MEM_EQ(read_buffer, data_b, 1000);
    ASSERT_EQ(free_clusters(&fat), (uint16_t)(FAT12_DATA_CLUSTERS - 2u));
  }
}

TEST(test_abort_is_safe_only_before_commit) {
  mount_clean();
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "DROP.BIN", &writer), DISK_OK);
  fill_pattern(data_a, 40000, 8);
  disk_result_t result = fat12_write(&writer, data_a, 40000);
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(fat12_abort_write(&writer), DISK_OK);
  ASSERT(!fat12_busy(&fat));
  ASSERT(!cache_dirty(&cache));
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "DROP.BIN", &entry), DISK_ERR_NOT_FOUND);
  ASSERT_EQ(free_clusters(&fat), FAT12_DATA_CLUSTERS);

  ASSERT_EQ(fat12_open_write(&fat, "MEDIA.BIN", &writer), DISK_OK);
  result = fat12_write(&writer, data_a, 1000);
  ASSERT_EQ(result.error, DISK_OK);
  fat12_forget_write(&writer);
  ASSERT(!fat12_busy(&fat));
  ASSERT_NULL(writer.fat);
  ASSERT_EQ(fat12_find(&fat, "MEDIA.BIN", &entry), DISK_ERR_NOT_FOUND);
}

TEST(test_full_disk_abort_never_publishes_fat_metadata) {
  mount_clean();
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "ABORTALL.BIN", &writer), DISK_OK);
  uint8_t cluster[DISK_SECTOR_SIZE];
  memset(cluster, 0xC3, sizeof(cluster));
  int writes = disk.track_writes;
  for (uint16_t index = 0; index < FAT12_DATA_CLUSTERS; index++) {
    disk_result_t result = fat12_write(&writer, cluster, sizeof(cluster));
    ASSERT_EQ(result.error, DISK_OK);
    ASSERT_EQ(result.count, sizeof(cluster));
  }
  ASSERT((unsigned)(disk.track_writes - writes) <= DISK_TRACK_COUNT);
  ASSERT_EQ(fat12_abort_write(&writer), DISK_OK);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 2), 0);
  ASSERT_EQ(free_clusters(&fat), FAT12_DATA_CLUSTERS);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "ABORTALL.BIN", &entry), DISK_ERR_NOT_FOUND);
}

TEST(test_copy_on_write_full_disk_preserves_old_file) {
  mount_clean();
  fill_pattern(data_a, 1024, 9);
  fill_pattern(data_b, 1024, 10);
  write_file(&fat, "TARGET.BIN", data_a, 1024);

  static uint8_t cluster[DISK_SECTOR_SIZE];
  memset(cluster, 0xA5, sizeof(cluster));
  fat12_writer_t filler;
  ASSERT_EQ(fat12_open_write(&fat, "FILL.BIN", &filler), DISK_OK);
  uint16_t available = free_clusters(&fat);
  for (uint16_t index = 0; index < available; index++) {
    disk_result_t result = fat12_write(&filler, cluster, sizeof(cluster));
    ASSERT_EQ(result.error, DISK_OK);
    ASSERT_EQ(result.count, sizeof(cluster));
  }
  ASSERT_EQ(fat12_close_write(&filler), DISK_OK);
  ASSERT_EQ(free_clusters(&fat), 0);

  fat12_writer_t replacement;
  ASSERT_EQ(fat12_open_write(&fat, "TARGET.BIN", &replacement), DISK_OK);
  disk_result_t result = fat12_write(&replacement, data_b, 1024);
  ASSERT_EQ(result.error, DISK_ERR_FULL);
  ASSERT_EQ(result.count, 0);
  ASSERT_EQ(fat12_abort_write(&replacement), DISK_OK);
  ASSERT_EQ(read_file(&fat, "TARGET.BIN", read_buffer, sizeof(read_buffer)), 1024);
  ASSERT_MEM_EQ(read_buffer, data_a, 1024);
}

TEST(test_allocation_wraps_complete_cluster_space) {
  vdisk_format_valid(&disk);
  uint16_t last = FAT12_CLUSTER_LIMIT - 1u;
  vdisk_set_fat_entry(&disk, last, 0xFFF);
  mount(&disk);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "WRAP.BIN", &writer), DISK_OK);
  writer.first_cluster = last;
  writer.prev_cluster = last;
  writer.bytes_written = DISK_SECTOR_SIZE;
  disk_result_t result = fat12_write(&writer, (const uint8_t *)"x", 1);
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, 1);
  ASSERT_EQ(fat12_close_write(&writer), DISK_OK);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "WRAP.BIN", &entry), DISK_OK);
  ASSERT_EQ(entry.start_cluster, last);
  ASSERT_EQ(entry_next(&fat, last), 2);
}

TEST(test_new_allocation_rejects_referenced_free_cluster) {
  mount_clean();
  raw_dirent(&disk, 0, "LIVE    ", "BIN", FAT12_ATTR_ARCHIVE,
             2, DISK_SECTOR_SIZE);
  memset(disk.data[FAT12_DATA_START], 0x6D, DISK_SECTOR_SIZE);
  reload();
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 2), 0);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "NEW.BIN", &writer), DISK_ERR_CORRUPT);
  ASSERT_EQ(disk.data[FAT12_DATA_START][0], 0x6D);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 2), 0);
}

TEST(test_creation_advances_root_end_marker) {
  mount_clean();
  set_fat(&disk, 200, 0xFFF);
  raw_dirent(&disk, 1, "GHOST   ", "BIN", FAT12_ATTR_ARCHIVE,
             200, DISK_SECTOR_SIZE);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "NEW.BIN", &writer), DISK_OK);
  ASSERT_EQ(fat12_close_write(&writer), DISK_OK);
  ASSERT_EQ(disk.data[FAT12_ROOT_START][FAT12_DIR_ENTRY_SIZE], FAT12_DIRENT_END);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "NEW.BIN", &entry), DISK_OK);
  ASSERT_EQ(fat12_find(&fat, "GHOST.BIN", &entry), DISK_ERR_NOT_FOUND);
}

TEST(test_read_only_entries_are_enforced) {
  mount_clean();
  raw_dirent(&disk, 0, "LOCKED  ", "BIN",
             FAT12_ATTR_ARCHIVE | FAT12_ATTR_READ_ONLY, 0, 0);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "LOCKED.BIN", &writer), DISK_ERR_READ_ONLY);
  ASSERT_EQ(fat12_delete(&fat, "LOCKED.BIN"), DISK_ERR_READ_ONLY);
  ASSERT_EQ(fat12_rename(&fat, "LOCKED.BIN", "OTHER.BIN"), DISK_ERR_READ_ONLY);
}

TEST(test_mutators_refuse_crosslinked_reclamation) {
  mount_clean();
  fill_pattern(data_a, 1024, 20);
  write_file(&fat, "FIRST.BIN", data_a, 1024);
  fat12_dirent_t first;
  ASSERT_EQ(fat12_find(&fat, "FIRST.BIN", &first), DISK_OK);
  raw_dirent(&disk, 1, "SECOND  ", "BIN", FAT12_ATTR_ARCHIVE,
             first.start_cluster, first.size);

  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "FIRST.BIN", &writer), DISK_ERR_CORRUPT);
  ASSERT_EQ(fat12_delete(&fat, "FIRST.BIN"), DISK_ERR_CORRUPT);
  ASSERT_EQ(read_file(&fat, "FIRST.BIN", read_buffer, sizeof(read_buffer)), 1024);
  ASSERT_MEM_EQ(read_buffer, data_a, 1024);
  ASSERT_EQ(read_file(&fat, "SECOND.BIN", read_buffer, sizeof(read_buffer)), 1024);
  ASSERT_MEM_EQ(read_buffer, data_a, 1024);

  mount_clean();
  write_file(&fat, "FIRST.BIN", data_a, 1024);
  ASSERT_EQ(fat12_find(&fat, "FIRST.BIN", &first), DISK_OK);
  set_fat(&disk, 100, 0xFFF);
  raw_dirent(&disk, 1, "SUBDIR  ", "   ", FAT12_ATTR_DIRECTORY, 100, 0);
  memset(disk.data[FAT12_DATA_START + 98], 0, DISK_SECTOR_SIZE);
  raw_dirent_at(&disk, FAT12_DATA_START + 98, 0,
                "INNER   ", "BIN", FAT12_ATTR_ARCHIVE,
                first.start_cluster, first.size);
  ASSERT_EQ(fat12_open_write(&fat, "FIRST.BIN", &writer), DISK_ERR_CORRUPT);
  ASSERT_EQ(fat12_delete(&fat, "FIRST.BIN"), DISK_ERR_CORRUPT);
  ASSERT_EQ(read_file(&fat, "FIRST.BIN", read_buffer, sizeof(read_buffer)), 1024);
  ASSERT_MEM_EQ(read_buffer, data_a, 1024);
}

TEST(test_fsck_repairs_lost_clusters_and_fat_copies) {
  mount_clean();
  set_fat(&disk, 100, 101);
  set_fat(&disk, 101, 0xFFF);
  set_fat(&disk, 700, 0xFFF);
  disk.data[FAT12_FAT2_START + 8][400] ^= 0x42;
  reload();

  fat12_fsck_t report = check(&fat);
  ASSERT_EQ(report.lost_clusters, 3);
  ASSERT(report.fat_mismatch);
  report = repair(&fat);
  ASSERT_EQ(report.freed, 3);
  ASSERT(report.repaired_fat2);
  report = check(&fat);
  ASSERT_EQ(report.lost_clusters, 0);
  ASSERT(!report.fat_mismatch);
}

TEST(test_fsck_selects_the_consistent_fat_copy) {
  for (uint8_t damaged = 0; damaged < 2; damaged++) {
    mount_clean();
    fill_pattern(data_a, 1536, 22u + damaged);
    write_file(&fat, "MIRROR.BIN", data_a, 1536);
    fat12_dirent_t entry;
    ASSERT_EQ(fat12_find(&fat, "MIRROR.BIN", &entry), DISK_OK);
    uint16_t damaged_start = damaged == 0 ? FAT12_FAT1_START : FAT12_FAT2_START;
    vdisk_set_fat_copy_entry(&disk, damaged_start, entry.start_cluster, 0xFFF);
    mount(&disk);
    ASSERT_EQ(fat.fat_start,
              damaged == 0 ? FAT12_FAT2_START : FAT12_FAT1_START);
    ASSERT(check(&fat).fat_mismatch);
    ASSERT_EQ(read_file(&fat, "MIRROR.BIN", read_buffer,
                        sizeof(read_buffer)), 1536);
    ASSERT_MEM_EQ(read_buffer, data_a, 1536);

    fat12_fsck_t report = repair(&fat);
    ASSERT_EQ(report.authoritative_fat, damaged == 0 ? 2 : 1);
    ASSERT_EQ(report.repaired_fat1, damaged == 0);
    ASSERT_EQ(report.repaired_fat2, damaged != 0);
    ASSERT_EQ(read_file(&fat, "MIRROR.BIN", read_buffer,
                        sizeof(read_buffer)), 1536);
    ASSERT_MEM_EQ(read_buffer, data_a, 1536);
    ASSERT_EQ(vdisk_get_fat_copy_entry(
                  &disk, FAT12_FAT1_START, entry.start_cluster),
              vdisk_get_fat_copy_entry(
                  &disk, FAT12_FAT2_START, entry.start_cluster));
  }
}

TEST(test_fsck_refuses_ambiguous_fat_copies_without_writes) {
  mount_clean();
  fill_pattern(data_a, 1024, 24);
  write_file(&fat, "CHOICE.BIN", data_a, 1024);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "CHOICE.BIN", &entry), DISK_OK);
  uint16_t second = entry_next(&fat, entry.start_cluster);
  uint16_t alternate = second + 1u;
  set_fat_copy(&disk, FAT12_FAT2_START, entry.start_cluster, alternate);
  set_fat_copy(&disk, FAT12_FAT2_START, second, 0);
  set_fat_copy(&disk, FAT12_FAT2_START, alternate, 0xFFF);
  memcpy(data_b, disk.data[FAT12_FAT1_START],
         FAT12_NUM_FATS * FAT12_SECTORS_PER_FAT * DISK_SECTOR_SIZE);
  int writes = disk.track_writes;

  fat12_t candidate;
  ASSERT_EQ(mount_into(&candidate, &disk), DISK_ERR_AMBIGUOUS);
  ASSERT_EQ(disk.track_writes, writes);

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), DISK_ERR_AMBIGUOUS);
  ASSERT(report.fat_ambiguous);
  ASSERT_EQ(report.authoritative_fat, 0);
  ASSERT_EQ(disk.track_writes, writes);
  ASSERT_MEM_EQ(data_b, disk.data[FAT12_FAT1_START],
                FAT12_NUM_FATS * FAT12_SECTORS_PER_FAT * DISK_SECTOR_SIZE);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "BLOCKED.BIN", &writer),
            DISK_ERR_AMBIGUOUS);
  ASSERT_EQ(disk.track_writes, writes);
}

TEST(test_fsck_preserves_bad_cluster_markers) {
  mount_clean();
  set_fat(&disk, 100, 0xFF7);
  raw_dirent(&disk, 0, "BADLAST ", "BIN", FAT12_ATTR_ARCHIVE,
             100, DISK_SECTOR_SIZE);
  set_fat(&disk, 110, 111);
  set_fat(&disk, 111, 0xFF7);
  raw_dirent(&disk, 1, "BADMID  ", "BIN", FAT12_ATTR_ARCHIVE,
             110, DISK_SECTOR_SIZE * 2u);

  repair(&fat);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 100), 0xFF7);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT2_START, 100), 0xFF7);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 111), 0xFF7);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT2_START, 111), 0xFF7);
  ASSERT(fat12_is_eof(vdisk_get_fat_copy_entry(
      &disk, FAT12_FAT1_START, 110)));
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "BADLAST.BIN", &entry), DISK_OK);
  ASSERT_EQ(entry.start_cluster, 0);
  ASSERT_EQ(entry.size, 0);
  ASSERT_EQ(fat12_find(&fat, "BADMID.BIN", &entry), DISK_OK);
  ASSERT_EQ(entry.start_cluster, 110);
  ASSERT_EQ(entry.size, DISK_SECTOR_SIZE);
  fat12_fsck_t report = check(&fat);
  ASSERT_EQ(report.broken_chains, 0);
  ASSERT_EQ(report.size_mismatches, 0);
}

TEST(test_fsck_repairs_loop_and_short_size) {
  mount_clean();
  fill_pattern(data_a, 1536, 11);
  write_file(&fat, "LOOP.BIN", data_a, 1536);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "LOOP.BIN", &entry), DISK_OK);
  uint16_t second = entry_next(&fat, entry.start_cluster);
  uint16_t third = entry_next(&fat, second);
  set_fat(&disk, third, second);

  fat12_fsck_t report = check(&fat);
  ASSERT_EQ(report.loops, 1);
  ASSERT(report.broken_chains > 0);
  repair(&fat);
  report = check(&fat);
  ASSERT_EQ(report.loops, 0);
  ASSERT_EQ(report.broken_chains, 0);

  set_fat(&disk, entry.start_cluster, 0xFFF);
  report = repair(&fat);
  ASSERT(report.truncated_files > 0);
  ASSERT_EQ(fat12_find(&fat, "LOOP.BIN", &entry), DISK_OK);
  ASSERT_EQ(entry.size, DISK_SECTOR_SIZE);
  report = check(&fat);
  ASSERT_EQ(report.size_mismatches, 0);
  ASSERT_EQ(report.lost_clusters, 0);
}

TEST(test_fsck_repairs_file_crosslinks_deterministically) {
  mount_clean();
  fill_pattern(data_a, 1024, 12);
  write_file(&fat, "FIRST.BIN", data_a, 1024);
  fat12_dirent_t first;
  ASSERT_EQ(fat12_find(&fat, "FIRST.BIN", &first), DISK_OK);
  raw_dirent(&disk, 1, "SECOND  ", "BIN", FAT12_ATTR_ARCHIVE,
             first.start_cluster, first.size);

  fat12_fsck_t report = check(&fat);
  ASSERT_EQ(report.crosslinked, 1);
  repair(&fat);
  report = check(&fat);
  ASSERT_EQ(report.crosslinked, 0);
  fat12_dirent_t second;
  ASSERT_EQ(fat12_find(&fat, "SECOND.BIN", &second), DISK_OK);
  ASSERT_EQ(second.start_cluster, 0);
  ASSERT_EQ(second.size, 0);
  ASSERT_EQ(read_file(&fat, "FIRST.BIN", read_buffer, sizeof(read_buffer)), 1024);
  ASSERT_MEM_EQ(read_buffer, data_a, 1024);
}

TEST(test_fsck_repairs_crosslink_after_unique_prefix) {
  mount_clean();
  fill_pattern(data_a, 1024, 13);
  write_file(&fat, "FIRST.BIN", data_a, 1024);
  fat12_dirent_t first;
  ASSERT_EQ(fat12_find(&fat, "FIRST.BIN", &first), DISK_OK);
  uint16_t shared = entry_next(&fat, first.start_cluster);
  set_fat(&disk, 200, shared);
  raw_dirent(&disk, 1, "SECOND  ", "BIN", FAT12_ATTR_ARCHIVE, 200, 1024);

  repair(&fat);
  fat12_fsck_t report = check(&fat);
  ASSERT_EQ(report.crosslinked, 0);
  ASSERT_EQ(report.size_mismatches, 0);
  fat12_dirent_t second;
  ASSERT_EQ(fat12_find(&fat, "SECOND.BIN", &second), DISK_OK);
  ASSERT_EQ(second.start_cluster, 200);
  ASSERT_EQ(second.size, DISK_SECTOR_SIZE);
  ASSERT(fat12_is_eof(entry_next(&fat, 200)));
}

TEST(test_fsck_repairs_excess_tail_and_zero_size_chain) {
  mount_clean();
  fill_pattern(data_a, 512, 14);
  write_file(&fat, "TAIL.BIN", data_a, 512);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "TAIL.BIN", &entry), DISK_OK);
  set_fat(&disk, entry.start_cluster, 300);
  set_fat(&disk, 300, 0xFFF);
  set_fat(&disk, 400, 0xFFF);
  raw_dirent(&disk, 1, "ZERO    ", "BIN", FAT12_ATTR_ARCHIVE, 400, 0);
  set_fat(&disk, 500, 0xFFF);
  raw_dirent(&disk, 2, "HUGE    ", "BIN", FAT12_ATTR_ARCHIVE,
             500, UINT32_MAX);

  fat12_fsck_t report = repair(&fat);
  ASSERT(report.freed >= 2);
  ASSERT(report.freed_tails > 0);
  report = check(&fat);
  ASSERT_EQ(report.lost_clusters, 0);
  ASSERT_EQ(report.size_mismatches, 0);
  fat12_dirent_t zero;
  ASSERT_EQ(fat12_find(&fat, "ZERO.BIN", &zero), DISK_OK);
  ASSERT_EQ(zero.start_cluster, 0);
  fat12_dirent_t huge;
  ASSERT_EQ(fat12_find(&fat, "HUGE.BIN", &huge), DISK_OK);
  ASSERT_EQ(huge.size, DISK_SECTOR_SIZE);
}

TEST(test_fsck_traverses_forty_directories) {
  mount_clean();
  for (uint16_t index = 0; index < 40; index++) {
    uint16_t cluster = 2 + index;
    vdisk_set_fat_entry(&disk, cluster, 0xFFF);
    char name[9];
    snprintf(name, sizeof(name), "D%07u", index);
    raw_dirent(&disk, index, name, "   ", FAT12_ATTR_DIRECTORY,
               cluster, 0);
    memset(disk.data[FAT12_DATA_START + cluster - 2], 0, DISK_SECTOR_SIZE);
  }
  reload();
  fat12_fsck_t report = check(&fat);
  ASSERT_EQ(report.directories, 40);
  ASSERT_EQ(report.lost_clusters, 0);
}

TEST(test_fsck_removes_duplicate_directory_reference) {
  mount_clean();
  set_fat(&disk, 2, 0xFFF);
  memset(disk.data[FAT12_DATA_START], 0, DISK_SECTOR_SIZE);
  raw_dirent(&disk, 0, "FIRST   ", "   ", FAT12_ATTR_DIRECTORY, 2, 0);
  raw_dirent(&disk, 1, "SECOND  ", "   ", FAT12_ATTR_DIRECTORY, 2, 0);
  fat12_fsck_t report = repair(&fat);
  ASSERT_EQ(report.removed_directories, 1);
  report = check(&fat);
  ASSERT_EQ(report.crosslinked, 0);
  ASSERT_EQ(report.directories, 1);
}

TEST(test_fsck_removes_later_duplicate_name_and_reclaims_chain) {
  mount_clean();
  fill_pattern(data_a, DISK_SECTOR_SIZE, 25);
  write_file(&fat, "SAME.BIN", data_a, DISK_SECTOR_SIZE);
  set_fat(&disk, 200, 0xFFF);
  memset(disk.data[FAT12_DATA_START + 198], 0xB7, DISK_SECTOR_SIZE);
  raw_dirent(&disk, 1, "SAME    ", "BIN", FAT12_ATTR_ARCHIVE,
             200, DISK_SECTOR_SIZE);

  fat12_fsck_t report = check(&fat);
  ASSERT_EQ(report.duplicate_names, 1);
  ASSERT_EQ(report.lost_clusters, 0);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "OTHER.BIN", &writer), DISK_ERR_CORRUPT);

  report = repair(&fat);
  ASSERT_EQ(report.duplicate_names, 1);
  ASSERT_EQ(report.removed_duplicates, 1);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 200), 0);
  ASSERT_EQ(disk.data[FAT12_ROOT_START][FAT12_DIR_ENTRY_SIZE], FAT12_DIRENT_FREE);
  ASSERT_EQ(read_file(&fat, "SAME.BIN", read_buffer,
                      sizeof(read_buffer)), DISK_SECTOR_SIZE);
  ASSERT_MEM_EQ(read_buffer, data_a, DISK_SECTOR_SIZE);
  report = check(&fat);
  ASSERT_EQ(report.duplicate_names, 0);
  ASSERT_EQ(report.lost_clusters, 0);
}

TEST(test_duplicate_repair_removes_name_before_reclaim) {
  mount_clean();
  fill_pattern(data_a, DISK_SECTOR_SIZE, 26);
  write_file(&fat, "SAME.BIN", data_a, DISK_SECTOR_SIZE);
  set_fat(&disk, 200, 0xFFF);
  raw_dirent(&disk, 1, "SAME    ", "BIN", FAT12_ATTR_ARCHIVE,
             200, DISK_SECTOR_SIZE);
  disk.writes_before_failure = 1;

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), DISK_ERR_VERIFY);
  ASSERT(!fat12_busy(&fat));
  ASSERT_EQ(disk.data[FAT12_ROOT_START][FAT12_DIR_ENTRY_SIZE], FAT12_DIRENT_FREE);
  ASSERT(fat12_is_eof(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 200)));

  disk.writes_before_failure = -1;
  mount(&disk);
  ASSERT_EQ(read_file(&fat, "SAME.BIN", read_buffer,
                      sizeof(read_buffer)), DISK_SECTOR_SIZE);
  ASSERT_MEM_EQ(read_buffer, data_a, DISK_SECTOR_SIZE);
  repair(&fat);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 200), 0);
}

TEST(test_fsck_multi_action_repair_plan_is_stable) {
  mount_clean();
  fill_pattern(data_a, 2000, 15);
  write_file(&fat, "BROKEN.BIN", data_a, 2000);
  for (uint16_t cluster = 50; cluster < 900; cluster += 73) {
    vdisk_set_fat_entry(&disk, cluster, 0xFFF);
  }
  fat12_dirent_t entry;
  reload();
  ASSERT_EQ(fat12_find(&fat, "BROKEN.BIN", &entry), DISK_OK);
  set_fat(&disk, entry.start_cluster, 0xFFF);

  fat12_fsck_t report = repair(&fat);
  ASSERT(report.freed > 5);
  ASSERT(report.truncated_files > 0);
  report = check(&fat);
  ASSERT_EQ(report.lost_clusters, 0);
  ASSERT_EQ(report.broken_chains, 0);
  ASSERT_EQ(report.size_mismatches, 0);
  ASSERT(!report.fat_mismatch);
}

TEST(test_fsck_read_failure_never_mutates) {
  vdisk_format_valid(&disk);
  vdisk_set_fat_entry(&disk, 100, 0xFFF);
  mount(&disk);
  int writes = disk.track_writes;
  reload();
  disk.reads_before_failure = 0;
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), DISK_ERR_TIMEOUT);
  ASSERT_EQ(disk.track_writes, writes);
  ASSERT(!fat12_busy(&fat));
}

TEST(test_fsck_finishes_directory_reads_before_writes) {
  vdisk_format_valid(&disk);
  vdisk_set_fat_entry(&disk, 2, 0xFFF);
  vdisk_set_fat_entry(&disk, 100, 0xFFF);
  raw_dirent(&disk, 0, "DIRONE  ", "   ", FAT12_ATTR_DIRECTORY, 2, 0);
  raw_dirent(&disk, 1, "DIRTWO  ", "   ", FAT12_ATTR_DIRECTORY, 100, 0);
  uint16_t first = FAT12_DATA_START;
  uint16_t second = FAT12_DATA_START + 98u;
  raw_dirent_at(&disk, first, 0, "SAME    ", "BIN",
                FAT12_ATTR_ARCHIVE, 0, 0);
  raw_dirent_at(&disk, first, FAT12_DIR_ENTRY_SIZE, "SAME    ", "BIN",
                FAT12_ATTR_ARCHIVE, 0, 0);
  raw_dirent_at(&disk, second, 0, "SAME    ", "BIN",
                FAT12_ATTR_ARCHIVE, 0, 0);
  raw_dirent_at(&disk, second, FAT12_DIR_ENTRY_SIZE, "SAME    ", "BIN",
                FAT12_ATTR_ARCHIVE, 0, 0);
  mount(&disk);
  disk.fail_reads_after_write = true;

  fat12_fsck_t report = repair(&fat);
  ASSERT_EQ(report.removed_duplicates, 2);
  ASSERT_EQ(disk.data[first][FAT12_DIR_ENTRY_SIZE], FAT12_DIRENT_FREE);
  ASSERT_EQ(disk.data[second][FAT12_DIR_ENTRY_SIZE], FAT12_DIRENT_FREE);
  ASSERT_EQ(disk.track_writes, 2);

  disk.fail_reads_after_write = false;
  report = check(&fat);
  ASSERT_EQ(report.duplicate_names, 0);
  ASSERT_EQ(report.lost_clusters, 0);
}

TEST(test_fsck_repairs_many_duplicates_in_one_pass) {
  mount_clean();
  enum { DIRECTORY_COUNT = 20 };
  for (uint16_t index = 0; index < DIRECTORY_COUNT; index++) {
    uint16_t cluster = (uint16_t)(index + 2u);
    vdisk_set_fat_entry(&disk, cluster, 0xFFF);
    char name[9];
    snprintf(name, sizeof(name), "D%07u", index);
    raw_dirent(&disk, index, name, "   ", FAT12_ATTR_DIRECTORY, cluster, 0);
    uint16_t lba = (uint16_t)(FAT12_DATA_START + cluster - 2u);
    raw_dirent_at(&disk, lba, 0, "SAME    ", "BIN",
                  FAT12_ATTR_ARCHIVE, 0, 0);
    raw_dirent_at(&disk, lba, FAT12_DIR_ENTRY_SIZE, "SAME    ", "BIN",
                  FAT12_ATTR_ARCHIVE, 0, 0);
  }

  fat12_fsck_t report = repair(&fat);
  ASSERT_EQ(report.removed_duplicates, DIRECTORY_COUNT);
  report = check(&fat);
  ASSERT_EQ(report.duplicate_names, 0);
  ASSERT_EQ(report.lost_clusters, 0);
  for (uint16_t index = 0; index < DIRECTORY_COUNT; index++) {
    uint16_t lba = (uint16_t)(FAT12_DATA_START + index);
    ASSERT_EQ(disk.data[lba][FAT12_DIR_ENTRY_SIZE], FAT12_DIRENT_FREE);
  }
}

TEST(test_fsck_bad_union_survives_duplicate_removal) {
  mount_clean();
  set_fat(&disk, 2, 0xFFF);
  set_fat(&disk, 200, 0xFFF);
  set_fat_copy(&disk, FAT12_FAT2_START, 200, 0xFF7);
  raw_dirent(&disk, 0, "SAME    ", "BIN", FAT12_ATTR_ARCHIVE,
             2, DISK_SECTOR_SIZE);
  raw_dirent(&disk, 1, "SAME    ", "BIN", FAT12_ATTR_ARCHIVE,
             200, DISK_SECTOR_SIZE);

  fat12_fsck_t report = repair(&fat);
  ASSERT_EQ(report.removed_duplicates, 1);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 200), 0xFF7);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT2_START, 200), 0xFF7);
  report = check(&fat);
  ASSERT_EQ(report.duplicate_names, 0);
  ASSERT_EQ(report.lost_clusters, 0);
}

TEST(test_fsck_repairs_identical_invalid_markers) {
  vdisk_format_valid(&disk);
  disk.data[FAT12_FAT1_START][0] = 0;
  disk.data[FAT12_FAT2_START][0] = 0;
  fat12_t candidate;
  ASSERT_EQ(mount_into(&candidate, &disk), DISK_OK);

  fat12_fsck_t report = check(&candidate);
  ASSERT(report.fat_markers_invalid);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&candidate, "BLOCKED.BIN", &writer),
            DISK_ERR_CORRUPT);
  report = repair(&candidate);
  ASSERT(report.repaired_fat1);
  ASSERT(report.repaired_fat2);
  ASSERT_EQ(disk.data[FAT12_FAT1_START][0], FAT12_MEDIA_DESCRIPTOR);
  ASSERT_EQ(disk.data[FAT12_FAT2_START][0], FAT12_MEDIA_DESCRIPTOR);
  ASSERT_EQ(mount_into(&candidate, &disk), DISK_OK);
  ASSERT(!check(&candidate).fat_markers_invalid);
}

TEST(test_marker_repair_converges_across_write_failure) {
  for (uint8_t apply = 0; apply < 2; apply++) {
    vdisk_format_valid(&disk);
    disk.data[FAT12_FAT1_START][0] = 0;
    disk.data[FAT12_FAT2_START][0] = 0;
    mount(&disk);
    disk.writes_before_failure = 0;
    disk.apply_failed_write = apply != 0;
    fat12_fsck_t report;
    ASSERT_EQ(fat12_fsck(&fat, &report, true), DISK_ERR_VERIFY);

    disk.writes_before_failure = -1;
    disk.apply_failed_write = false;
    mount(&disk);
    repair(&fat);
    mount(&disk);
    ASSERT(!check(&fat).fat_markers_invalid);
    ASSERT_EQ(disk.data[FAT12_FAT1_START][0], FAT12_MEDIA_DESCRIPTOR);
    ASSERT_EQ(disk.data[FAT12_FAT2_START][0], FAT12_MEDIA_DESCRIPTOR);
  }

  vdisk_format_valid(&disk);
  disk.data[FAT12_FAT1_START][0] = 0;
  disk.data[FAT12_FAT2_START][0] = 0;
  mount(&disk);
  disk.tear_next_write = true;
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), DISK_ERR_IO);
  mount(&disk);
  repair(&fat);
  mount(&disk);
  ASSERT(!check(&fat).fat_markers_invalid);
}

TEST(test_fsck_cuts_directory_chain_before_bad_cluster) {
  mount_clean();
  make_dir(0, "DIRA    ", 10);
  set_fat(&disk, 10, 11);
  set_fat(&disk, 11, 0xFF7);
  fat12_fsck_t report = check(&fat);
  ASSERT_EQ(report.broken_chains, 1);
  ASSERT_EQ(report.directories, 1);
  report = repair(&fat);
  ASSERT_EQ(report.removed_directories, 0);
  ASSERT(fat12_is_eof(entry_next(&fat, 10)));
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 11), 0xFF7);
  expect_clean(&fat);
}

TEST(test_fsck_removes_directory_starting_in_bad_cluster) {
  mount_clean();
  make_dir(0, "DIRBAD  ", 10);
  set_fat(&disk, 10, 0xFF7);
  fat12_fsck_t report = check(&fat);
  ASSERT_EQ(report.broken_chains, 1);
  report = repair(&fat);
  ASSERT_EQ(report.removed_directories, 1);
  ASSERT_EQ(disk.data[FAT12_ROOT_START][0], FAT12_DIRENT_FREE);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 10), 0xFF7);
  report = check(&fat);
  ASSERT(fat12_fsck_clean(&report));
  ASSERT_EQ(report.directories, 0);
}

TEST(test_fsck_removes_directory_with_invalid_start_cluster) {
  static const uint16_t starts[] = {0, 1, FAT12_CLUSTER_LIMIT, 0xFFF};
  for (size_t index = 0; index < sizeof(starts) / sizeof(starts[0]); index++) {
    mount_clean();
    raw_dirent(&disk, 0, "DIRNONE ", "   ", FAT12_ATTR_DIRECTORY,
               starts[index], 0);
    fat12_fsck_t report = check(&fat);
    ASSERT_EQ(report.broken_chains, 1);
    ASSERT_EQ(report.directories, 1);
    report = repair(&fat);
    ASSERT_EQ(report.removed_directories, 1);
    ASSERT_EQ(disk.data[FAT12_ROOT_START][0], FAT12_DIRENT_FREE);
    report = check(&fat);
    ASSERT(fat12_fsck_clean(&report));
    ASSERT_EQ(report.directories, 0);
  }
}

TEST(test_fsck_cuts_directory_chain_at_invalid_link) {
  mount_clean();
  make_dir(0, "DIRB    ", 10);
  set_fat(&disk, 10, 11);
  set_fat(&disk, 11, 0x0FF0);
  fat12_fsck_t report = check(&fat);
  ASSERT_EQ(report.broken_chains, 1);
  ASSERT_EQ(report.loops, 0);
  repair(&fat);
  ASSERT_EQ(entry_next(&fat, 10), 11);
  ASSERT(fat12_is_eof(entry_next(&fat, 11)));
  expect_clean(&fat);
}

TEST(test_fsck_cuts_looping_directory_chain) {
  mount_clean();
  make_dir(0, "DIRLOOP ", 10);
  set_fat(&disk, 10, 11);
  set_fat(&disk, 11, 10);
  fat12_fsck_t report = check(&fat);
  ASSERT_EQ(report.loops, 1);
  ASSERT_EQ(report.broken_chains, 1);
  repair(&fat);
  ASSERT_EQ(entry_next(&fat, 10), 11);
  ASSERT(fat12_is_eof(entry_next(&fat, 11)));
  expect_clean(&fat);
}

TEST(test_fsck_cuts_directory_chain_crossing_into_file) {
  mount_clean();
  fill_pattern(data_a, 1024, 27);
  write_file(&fat, "FILE.BIN", data_a, 1024);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "FILE.BIN", &entry), DISK_OK);
  uint16_t second = entry_next(&fat, entry.start_cluster);
  make_dir(1, "DIRX    ", 10);
  set_fat(&disk, 10, second);
  fat12_fsck_t report = check(&fat);
  ASSERT_EQ(report.crosslinked, 1);
  ASSERT_EQ(report.loops, 0);
  repair(&fat);
  ASSERT(fat12_is_eof(entry_next(&fat, 10)));
  ASSERT_EQ(entry_next(&fat, entry.start_cluster), second);
  ASSERT_EQ(read_file(&fat, "FILE.BIN", read_buffer, sizeof(read_buffer)), 1024);
  ASSERT_MEM_EQ(read_buffer, data_a, 1024);
  expect_clean(&fat);
}

TEST(test_fsck_removes_nested_duplicate_directory_reference) {
  mount_clean();
  make_dir(0, "OUTER   ", 2);
  make_dir(1, "SIBLING ", 3);
  raw_dirent_at(&disk, FAT12_DATA_START, 0, "INNER   ", "   ",
                FAT12_ATTR_DIRECTORY, 3, 0);
  fat12_fsck_t report = check(&fat);
  ASSERT_EQ(report.crosslinked, 1);
  ASSERT_EQ(report.directories, 3);
  report = repair(&fat);
  ASSERT_EQ(report.removed_directories, 1);
  ASSERT_EQ(disk.data[FAT12_DATA_START][0], FAT12_DIRENT_FREE);
  ASSERT_EQ(disk.data[FAT12_ROOT_START][0], 'O');
  report = check(&fat);
  ASSERT(fat12_fsck_clean(&report));
  ASSERT_EQ(report.directories, 2);
}

TEST(test_fsck_truncates_file_chain_at_invalid_link) {
  mount_clean();
  set_fat(&disk, 10, 11);
  set_fat(&disk, 11, 0x0FF0);
  raw_dirent(&disk, 0, "BROKEN  ", "BIN", FAT12_ATTR_ARCHIVE, 10,
             3u * DISK_SECTOR_SIZE);
  fat12_fsck_t report = check(&fat);
  ASSERT_EQ(report.broken_chains, 1);
  ASSERT_EQ(report.size_mismatches, 1);
  report = repair(&fat);
  ASSERT_EQ(report.truncated_files, 1);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "BROKEN.BIN", &entry), DISK_OK);
  ASSERT_EQ(entry.start_cluster, 10);
  ASSERT_EQ(entry.size, 2u * DISK_SECTOR_SIZE);
  ASSERT(fat12_is_eof(entry_next(&fat, 11)));
  expect_clean(&fat);
}

TEST(test_fsck_caps_oversized_file_at_disk_length) {
  static const uint16_t tails[] = {0x0FF0, 0xFFF};
  for (size_t index = 0; index < sizeof(tails) / sizeof(tails[0]); index++) {
    mount_clean();
    for (uint16_t cluster = 2; cluster + 1u < FAT12_CLUSTER_LIMIT; cluster++) {
      vdisk_set_fat_entry(&disk, cluster, (uint16_t)(cluster + 1u));
    }
    vdisk_set_fat_entry(&disk, FAT12_CLUSTER_LIMIT - 1u, tails[index]);
    raw_dirent(&disk, 0, "HUGE    ", "BIN", FAT12_ATTR_ARCHIVE, 2, UINT32_MAX);
    fat12_fsck_t report = check(&fat);
    ASSERT_EQ(report.broken_chains, 1);
    ASSERT_EQ(report.size_mismatches, 1);
    ASSERT_EQ(report.lost_clusters, 0);
    report = repair(&fat);
    ASSERT_EQ(report.truncated_files, 1);
    fat12_dirent_t entry;
    ASSERT_EQ(fat12_find(&fat, "HUGE.BIN", &entry), DISK_OK);
    ASSERT_EQ(entry.start_cluster, 2);
    ASSERT_EQ(entry.size, FAT12_DATA_CLUSTERS * DISK_SECTOR_SIZE);
    ASSERT(fat12_is_eof(entry_next(&fat, FAT12_CLUSTER_LIMIT - 1u)));
    expect_clean(&fat);
  }
}

TEST(test_fsck_clean_flags_every_field) {
  mount_clean();
  fat12_fsck_t report = check(&fat);
  ASSERT(fat12_fsck_clean(&report));
  set_fat(&disk, 100, 0xFFF);
  report = check(&fat);
  ASSERT_EQ(report.lost_clusters, 1);
  ASSERT(!fat12_fsck_clean(&report));
  fat12_fsck_t probe = {.crosslinked = 1};
  ASSERT(!fat12_fsck_clean(&probe));
  probe = (fat12_fsck_t){.loops = 1};
  ASSERT(!fat12_fsck_clean(&probe));
  probe = (fat12_fsck_t){.broken_chains = 1};
  ASSERT(!fat12_fsck_clean(&probe));
  probe = (fat12_fsck_t){.size_mismatches = 1};
  ASSERT(!fat12_fsck_clean(&probe));
  probe = (fat12_fsck_t){.truncated_files = 1};
  ASSERT(!fat12_fsck_clean(&probe));
  probe = (fat12_fsck_t){.duplicate_names = 1};
  ASSERT(!fat12_fsck_clean(&probe));
  probe = (fat12_fsck_t){.fat_mismatch = true};
  ASSERT(!fat12_fsck_clean(&probe));
  probe = (fat12_fsck_t){.fat_markers_invalid = true};
  ASSERT(!fat12_fsck_clean(&probe));
  probe = (fat12_fsck_t){.fat_ambiguous = true};
  ASSERT(!fat12_fsck_clean(&probe));
  probe = (fat12_fsck_t){.files = 5, .directories = 3, .freed = 2,
                         .freed_tails = 1, .removed_directories = 1,
                         .removed_duplicates = 1, .fat1_score = 7,
                         .fat2_score = 7, .authoritative_fat = 1,
                         .repaired_fat1 = true, .repaired_fat2 = true};
  ASSERT(fat12_fsck_clean(&probe));
}

TEST(test_fsck_repair_aborts_when_directory_pass_cannot_flush) {
  mount_clean();
  set_fat(&disk, 2, 0xFFF);
  set_fat(&disk, 200, 0xFFF);
  raw_dirent(&disk, 0, "SAME    ", "BIN", FAT12_ATTR_ARCHIVE, 2, DISK_SECTOR_SIZE);
  raw_dirent(&disk, 1, "SAME    ", "BIN", FAT12_ATTR_ARCHIVE, 200, DISK_SECTOR_SIZE);
  uint64_t digest = disk_digest(&disk);
  disk.writes_before_failure = 0;
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), DISK_ERR_VERIFY);
  ASSERT(!fat12_busy(&fat));
  ASSERT(!cache_dirty(&cache));
  ASSERT_EQ(disk_digest(&disk), digest);
  disk.writes_before_failure = -1;
  report = repair(&fat);
  ASSERT_EQ(report.removed_duplicates, 1);
  expect_clean(&fat);
}

TEST(test_fsck_picks_structurally_sound_fat_when_markers_are_invalid) {
  for (int variant = 0; variant < 5; variant++) {
    mount_clean();
    fill_pattern(data_a, 1024, 30u + (uint32_t)variant);
    write_file(&fat, "PAIR.BIN", data_a, 1024);
    fat12_dirent_t entry;
    ASSERT_EQ(fat12_find(&fat, "PAIR.BIN", &entry), DISK_OK);
    uint16_t start = entry.start_cluster;
    uint16_t second = entry_next(&fat, start);
    uint16_t damaged = variant == 1 ? FAT12_FAT1_START : FAT12_FAT2_START;
    disk.data[FAT12_FAT1_START][0] = 0;
    disk.data[FAT12_FAT2_START][0] = 0;
    switch (variant) {
      case 0:
      case 1:
        set_fat_copy(&disk, damaged, start, 0xFFF);
        set_fat_copy(&disk, damaged, second, 0);
        break;
      case 2:
        set_fat_copy(&disk, damaged, start, (uint16_t)(second + 1u));
        set_fat_copy(&disk, damaged, second, 0);
        set_fat_copy(&disk, damaged, (uint16_t)(second + 1u), 0xFFF);
        break;
      case 3:
        set_fat_copy(&disk, damaged, 100, 0xFFF);
        break;
      case 4:
        set_fat_copy(&disk, FAT12_FAT1_START, start, 0xFFF);
        set_fat_copy(&disk, FAT12_FAT1_START, second, 0);
        set_fat_copy(&disk, damaged, start, 0x0FF0);
        break;
    }
    reload();
    fat12_t candidate;
    disk_err_t mounted = mount_into(&candidate, &disk);
    fat12_fsck_t report;
    if (variant == 2 || variant == 4) {
      ASSERT_EQ(mounted, DISK_ERR_AMBIGUOUS);
      ASSERT_EQ(fat12_fsck(&fat, &report, false), DISK_ERR_AMBIGUOUS);
      ASSERT(report.fat_ambiguous);
      ASSERT(report.fat_markers_invalid);
      continue;
    }
    ASSERT_EQ(mounted, DISK_OK);
    report = check(&candidate);
    ASSERT_EQ(report.authoritative_fat, variant == 1 ? 2 : 1);
    ASSERT(report.fat_mismatch);
    ASSERT(report.fat_markers_invalid);
    ASSERT_EQ(read_file(&candidate, "PAIR.BIN", read_buffer,
                        sizeof(read_buffer)), 1024);
    ASSERT_MEM_EQ(read_buffer, data_a, 1024);
    report = repair(&candidate);
    ASSERT(report.repaired_fat1);
    ASSERT(report.repaired_fat2);
    mount(&disk);
    expect_clean(&fat);
    ASSERT_EQ(read_file(&fat, "PAIR.BIN", read_buffer, sizeof(read_buffer)), 1024);
    ASSERT_MEM_EQ(read_buffer, data_a, 1024);
  }
}

TEST(test_fsck_refuses_fat_copies_reaching_different_clusters) {
  mount_clean();
  set_fat(&disk, 10, 11);
  set_fat(&disk, 11, 0xFFF);
  set_fat_copy(&disk, FAT12_FAT2_START, 10, 3);
  set_fat_copy(&disk, FAT12_FAT2_START, 11, 0);
  set_fat_copy(&disk, FAT12_FAT2_START, 3, 0xFFF);
  raw_dirent(&disk, 0, "FORK    ", "BIN", FAT12_ATTR_ARCHIVE, 10, 1024);
  fat12_t candidate;
  ASSERT_EQ(mount_into(&candidate, &disk), DISK_ERR_AMBIGUOUS);
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), DISK_ERR_AMBIGUOUS);
  ASSERT(report.fat_ambiguous);
  ASSERT(!report.fat_markers_invalid);
  ASSERT_EQ(report.fat1_score, 0);
  ASSERT_EQ(report.fat2_score, 0);
  ASSERT_EQ(disk.track_writes, 0);
}

TEST(test_open_rejects_directory_and_volume_entries) {
  mount_clean();
  make_dir(0, "SUBDIR  ", 10);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "SUBDIR", &entry), DISK_OK);
  fat12_file_t file;
  ASSERT_EQ(fat12_open(&fat, &entry, &file), DISK_ERR_INVALID);
  entry.attr = FAT12_ATTR_VOLUME_ID;
  ASSERT_EQ(fat12_open(&fat, &entry, &file), DISK_ERR_INVALID);
}

TEST(test_open_rejects_damaged_chains) {
  static const struct {
    uint32_t size;
    uint16_t link;
  } cases[] = {
    {2u * DISK_SECTOR_SIZE, 0xFF7},
    {2u * DISK_SECTOR_SIZE, 0x0FF0},
    {2u * DISK_SECTOR_SIZE, 0xFFF},
    {DISK_SECTOR_SIZE, 11},
  };
  for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
    mount_clean();
    set_fat(&disk, 10, cases[index].link);
    set_fat(&disk, 11, 0xFFF);
    raw_dirent(&disk, 0, "DAMAGED ", "BIN", FAT12_ATTR_ARCHIVE, 10,
               cases[index].size);
    fat12_dirent_t entry;
    ASSERT_EQ(fat12_find(&fat, "DAMAGED.BIN", &entry), DISK_OK);
    fat12_file_t file;
    ASSERT_EQ(fat12_open(&fat, &entry, &file), DISK_ERR_CORRUPT);
  }
  mount_clean();
  raw_dirent(&disk, 0, "NOSTART ", "BIN", FAT12_ATTR_ARCHIVE, 0, DISK_SECTOR_SIZE);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "NOSTART.BIN", &entry), DISK_OK);
  fat12_file_t file;
  ASSERT_EQ(fat12_open(&fat, &entry, &file), DISK_ERR_CORRUPT);
  raw_dirent(&disk, 0, "TOOBIG  ", "BIN", FAT12_ATTR_ARCHIVE, 2, UINT32_MAX);
  ASSERT_EQ(fat12_find(&fat, "TOOBIG.BIN", &entry), DISK_OK);
  ASSERT_EQ(fat12_open(&fat, &entry, &file), DISK_ERR_CORRUPT);
}

TEST(test_read_and_seek_detect_chain_damaged_after_open) {
  mount_clean();
  fill_pattern(data_a, 1536, 28);
  write_file(&fat, "LIVE.BIN", data_a, 1536);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "LIVE.BIN", &entry), DISK_OK);
  uint16_t second = entry_next(&fat, entry.start_cluster);
  fat12_file_t file;
  ASSERT_EQ(fat12_open(&fat, &entry, &file), DISK_OK);

  set_fat(&disk, second, 0xFFF);
  ASSERT_EQ(fat12_seek(&file, 1024), DISK_ERR_CORRUPT);
  ASSERT_EQ(fat12_seek(&file, 0), DISK_OK);
  disk_result_t result = fat12_read(&file, read_buffer, 1536);
  ASSERT_EQ(result.error, DISK_ERR_CORRUPT);
  ASSERT_EQ(result.count, 1024);
  ASSERT_MEM_EQ(read_buffer, data_a, 1024);

  set_fat(&disk, entry.start_cluster, 0x0FF0);
  ASSERT_EQ(fat12_seek(&file, 0), DISK_OK);
  result = fat12_read(&file, read_buffer, 1536);
  ASSERT_EQ(result.error, DISK_ERR_CORRUPT);
  ASSERT_EQ(result.count, 0);
  ASSERT_EQ(fat12_seek(&file, 1024), DISK_ERR_CORRUPT);
}

TEST(test_writer_is_busy_after_partial_commit_and_remembers_full_disk) {
  mount_clean();
  uint8_t byte = 0x42;
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "LATE.BIN", &writer), DISK_OK);
  ASSERT_EQ(fat12_write(&writer, &byte, 1).error, DISK_OK);
  disk.writes_before_failure = 0;
  ASSERT_EQ(fat12_close_write(&writer), DISK_ERR_VERIFY);
  ASSERT_EQ(fat12_write(&writer, &byte, 1).error, DISK_ERR_BUSY);
  disk.writes_before_failure = -1;
  ASSERT_EQ(fat12_close_write(&writer), DISK_OK);
  ASSERT_EQ(fat12_close_write(&writer), DISK_OK);
  ASSERT_EQ(read_file(&fat, "LATE.BIN", read_buffer, sizeof(read_buffer)), 1);
  ASSERT_EQ(read_buffer[0], 0x42);

  vdisk_format_valid(&disk);
  for (uint16_t cluster = 2; cluster < FAT12_CLUSTER_LIMIT; cluster++) {
    vdisk_set_fat_entry(&disk, cluster, 0xFFF);
  }
  mount(&disk);
  ASSERT_EQ(free_clusters(&fat), 0);
  ASSERT_EQ(fat12_open_write(&fat, "NOROOM.BIN", &writer), DISK_OK);
  disk_result_t result = fat12_write(&writer, &byte, 1);
  ASSERT_EQ(result.error, DISK_ERR_FULL);
  ASSERT_EQ(result.count, 0);
  ASSERT_EQ(fat12_write(&writer, &byte, 1).error, DISK_ERR_FULL);
  ASSERT_EQ(fat12_close_write(&writer), DISK_ERR_FULL);
  ASSERT(fat12_busy(&fat));
  ASSERT_EQ(fat12_abort_write(&writer), DISK_OK);
  ASSERT(!fat12_busy(&fat));
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "NOROOM.BIN", &entry), DISK_ERR_NOT_FOUND);
}

TEST(test_allocation_wraps_around_used_tail) {
  vdisk_format_valid(&disk);
  uint16_t hole = FAT12_CLUSTER_LIMIT - 9u;
  for (uint16_t cluster = hole; cluster < FAT12_CLUSTER_LIMIT; cluster++) {
    vdisk_set_fat_entry(&disk, cluster, 0xFFF);
  }
  mount(&disk);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "WRAP.BIN", &writer), DISK_OK);
  writer.first_cluster = hole;
  writer.prev_cluster = hole;
  writer.bytes_written = DISK_SECTOR_SIZE;
  disk_result_t result = fat12_write(&writer, (const uint8_t *)"x", 1);
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, 1);
  ASSERT_EQ(fat12_close_write(&writer), DISK_OK);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "WRAP.BIN", &entry), DISK_OK);
  ASSERT_EQ(entry.start_cluster, hole);
  ASSERT_EQ(entry_next(&fat, hole), 2);
  ASSERT(fat12_is_eof(entry_next(&fat, 2)));
}

TEST(test_open_write_rejects_damaged_existing_chain) {
  mount_clean();
  raw_dirent(&disk, 0, "NOSTART ", "BIN", FAT12_ATTR_ARCHIVE, 0, 100);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "NOSTART.BIN", &writer), DISK_ERR_CORRUPT);

  static const uint16_t links[] = {0xFF7, 0x0FF0};
  for (size_t index = 0; index < sizeof(links) / sizeof(links[0]); index++) {
    mount_clean();
    set_fat(&disk, 10, links[index]);
    raw_dirent(&disk, 0, "BADLINK ", "BIN", FAT12_ATTR_ARCHIVE, 10, DISK_SECTOR_SIZE);
    ASSERT_EQ(fat12_open_write(&fat, "BADLINK.BIN", &writer), DISK_ERR_CORRUPT);
  }

  mount_clean();
  set_fat(&disk, 10, 11);
  set_fat(&disk, 11, 10);
  raw_dirent(&disk, 0, "ENDLESS ", "BIN", FAT12_ATTR_ARCHIVE, 10, 1024);
  ASSERT_EQ(fat12_open_write(&fat, "ENDLESS.BIN", &writer), DISK_ERR_CORRUPT);
  ASSERT(!fat12_busy(&fat));
  ASSERT_EQ(disk.track_writes, 0);
}

TEST(test_format_rejects_invalid_labels) {
  vdisk_init(&disk);
  ASSERT_EQ(cache_init(&cache, vdisk_device(&disk)), DISK_OK);
  ASSERT_EQ(cache_bind(&cache), DISK_OK);
  ASSERT_EQ(fat12_format(&fat, &cache, "BAD/NAME", false, NULL, NULL),
            DISK_ERR_INVALID);
  ASSERT_EQ(fat12_format(&fat, &cache, "TAB\tNAME", false, NULL, NULL),
            DISK_ERR_INVALID);
  ASSERT_EQ(fat12_format(&fat, &cache, "", false, NULL, NULL), DISK_ERR_INVALID);
  ASSERT_EQ(disk.track_writes, 0);
}

TEST(test_hardware_scenario_reports_and_repairs_exactly) {
  mount_clean();
  uint16_t next_cluster = 2;
  for (size_t index = 0; index < FSCK_SCENARIO_FILE_COUNT; index++) {
    const fsck_scenario_file_t *file = &fsck_scenario_files[index];
    fill_pattern(data_a, file->size, 40u + (uint32_t)index);
    write_file(&fat, file->name, data_a, file->size);
    fat12_dirent_t entry;
    ASSERT_EQ(fat12_find(&fat, file->name, &entry), DISK_OK);
    ASSERT_EQ(entry.start_cluster, next_cluster);
    next_cluster = (uint16_t)(next_cluster + file->size / DISK_SECTOR_SIZE);
  }
  for (size_t index = 0; index < FSCK_SCENARIO_PATCH_COUNT; index++) {
    set_fat(&disk, fsck_scenario_patches[index].cluster,
            fsck_scenario_patches[index].value);
  }
  fat12_fsck_t report = check(&fat);
  ASSERT(fsck_scenario_damage_matches(&report));
  report = repair(&fat);
  ASSERT(fsck_scenario_repair_matches(&report));
  expect_clean(&fat);
  for (size_t index = 0; index < FSCK_SCENARIO_FILE_COUNT; index++) {
    const fsck_scenario_file_t *file = &fsck_scenario_files[index];
    uint32_t size = index == FSCK_SCENARIO_TRUNCATED_FILE
        ? FSCK_SCENARIO_TRUNCATED_SIZE : file->size;
    fill_pattern(data_a, file->size, 40u + (uint32_t)index);
    ASSERT_EQ(read_file(&fat, file->name, read_buffer, sizeof(read_buffer)), size);
    ASSERT_MEM_EQ(read_buffer, data_a, size);
  }

  set_fat_copy(&disk, FAT12_FAT2_START, FSCK_SCENARIO_MIRROR_CLUSTER,
               FSCK_SCENARIO_MIRROR_VALUE);
  mount(&disk);
  report = check(&fat);
  ASSERT(fsck_scenario_mirror_matches(&report));
  report = repair(&fat);
  ASSERT(fsck_scenario_mirror_matches(&report));
  ASSERT(report.repaired_fat2);
  ASSERT(!report.repaired_fat1);
  expect_clean(&fat);
  fill_pattern(data_a, fsck_scenario_files[0].size, 40);
  ASSERT_EQ(read_file(&fat, fsck_scenario_files[0].name, read_buffer,
                      sizeof(read_buffer)), fsck_scenario_files[0].size);
  ASSERT_MEM_EQ(read_buffer, data_a, fsck_scenario_files[0].size);
}

static bool content_is(const uint8_t *expected, size_t length) {
  fat12_dirent_t entry;
  if (fat12_find(&fat, "POWER.BIN", &entry) != DISK_OK) return false;
  if (entry.size != length) return false;
  fat12_file_t file;
  if (fat12_open(&fat, &entry, &file) != DISK_OK) return false;
  disk_result_t result = fat12_read(&file, read_buffer, sizeof(read_buffer));
  return result.error == DISK_OK && result.count == length &&
      memcmp(read_buffer, expected, length) == 0;
}

TEST(test_verified_track_failure_boundaries_preserve_old_or_new) {
  fill_pattern(data_a, 1500, 16);
  fill_pattern(data_b, 1500, 17);
  for (int boundary = 0; boundary < 5; boundary++) {
    for (int apply = 0; apply < 2; apply++) {
      mount_clean();
      write_file(&fat, "POWER.BIN", data_a, 1500);

      fat12_writer_t writer;
      ASSERT_EQ(fat12_open_write(&fat, "POWER.BIN", &writer), DISK_OK);
      disk_result_t written = fat12_write(&writer, data_b, 1500);
      ASSERT_EQ(written.error, DISK_OK);
      disk.writes_before_failure = boundary;
      disk.apply_failed_write = apply != 0;
      disk_err_t closed = fat12_close_write(&writer);
      if (closed == DISK_OK) continue;
      ASSERT_EQ(closed, DISK_ERR_VERIFY);

      disk.writes_before_failure = -1;
      disk.apply_failed_write = false;
      mount(&disk);
      ASSERT(content_is(data_a, 1500) || content_is(data_b, 1500));
      repair(&fat);
      fat12_fsck_t report = check(&fat);
      ASSERT_EQ(report.lost_clusters, 0);
      ASSERT_EQ(report.broken_chains, 0);
      ASSERT_EQ(report.crosslinked, 0);
    }
  }
}

TEST(test_torn_metadata_track_is_detected_and_repaired) {
  mount_clean();
  fill_pattern(data_a, 1500, 18);
  fill_pattern(data_b, 1500, 19);
  write_file(&fat, "POWER.BIN", data_a, 1500);

  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "POWER.BIN", &writer), DISK_OK);
  disk_result_t written = fat12_write(&writer, data_b, 1500);
  ASSERT_EQ(written.error, DISK_OK);
  ASSERT_EQ(written.count, 1500);
  disk.tear_next_write = true;
  ASSERT_EQ(fat12_close_write(&writer), DISK_ERR_IO);

  mount(&disk);
  ASSERT(check(&fat).fat_mismatch);
  ASSERT(content_is(data_a, 1500));
  fat12_fsck_t report = repair(&fat);
  ASSERT(report.repaired_fat2);
  report = check(&fat);
  ASSERT_EQ(report.lost_clusters, 0);
  ASSERT(!report.fat_mismatch);
  ASSERT(content_is(data_a, 1500));
}

TEST(test_fsck_dry_run_is_repeatable_and_immutable) {
  mount_clean();
  set_fat(&disk, 100, 100);
  set_fat(&disk, 200, 0x0FFFu);
  raw_dirent(&disk, 0, "LOOP    ", "BIN", FAT12_ATTR_ARCHIVE, 100, 1024);

  uint64_t digest = disk_digest(&disk);
  int track_writes = disk.track_writes;
  fat12_fsck_t first = check(&fat);
  ASSERT_EQ(first.loops, 1);
  ASSERT_EQ(first.lost_clusters, 1);
  ASSERT(!first.fat_mismatch);
  ASSERT_EQ(disk.track_writes, track_writes);
  ASSERT_EQ(disk_digest(&disk), digest);

  fat12_fsck_t second = check(&fat);
  ASSERT_EQ(second.loops, first.loops);
  ASSERT_EQ(second.lost_clusters, first.lost_clusters);
  ASSERT_EQ(second.broken_chains, first.broken_chains);
  ASSERT_EQ(second.fat1_score, first.fat1_score);
  ASSERT_EQ(second.fat2_score, first.fat2_score);
  ASSERT_EQ(second.authoritative_fat, first.authoritative_fat);
  ASSERT_EQ(disk.track_writes, track_writes);
  ASSERT_EQ(disk_digest(&disk), digest);

  repair(&fat);
  first = check(&fat);
  ASSERT_EQ(first.loops, 0);
  ASSERT_EQ(first.lost_clusters, 0);
  ASSERT(!first.fat_mismatch);
}

static int progress_calls;
static uint16_t progress_done;
static uint16_t progress_total;

static void progress(void *ctx, uint8_t cylinder, uint8_t head,
                     uint16_t done, uint16_t total) {
  (void)ctx;
  ASSERT(disk_ch_valid(cylinder, head));
  ASSERT(done > progress_done);
  progress_calls++;
  progress_done = done;
  progress_total = total;
}

TEST(test_format_is_typed_and_writes_whole_metadata_tracks) {
  vdisk_init(&disk);
  ASSERT_EQ(cache_init(&cache, vdisk_readonly_device(&disk)), DISK_OK);
  ASSERT_EQ(cache_bind(&cache), DISK_OK);
  ASSERT_EQ(fat12_format(&fat, &cache, "TEST", false, NULL, NULL),
            DISK_ERR_WRITE_PROTECTED);
  ASSERT_EQ(cache_init(&cache, vdisk_device(&disk)), DISK_OK);
  ASSERT_EQ(cache_bind(&cache), DISK_OK);
  ASSERT_EQ(fat12_format(&fat, &cache, "LABEL-TOO-LONG", false, NULL, NULL),
            DISK_ERR_INVALID);
  progress_calls = 0;
  progress_done = 0;
  progress_total = 0;
  ASSERT_EQ(fat12_format(&fat, &cache, "TEST DISK", false, progress, NULL),
            DISK_OK);
  ASSERT_EQ(progress_calls, 2);
  ASSERT_EQ(progress_done, progress_total);
  ASSERT_EQ(disk.track_writes, 2);
  ASSERT_EQ(disk.track_reads, 0);
  ASSERT_EQ(disk.data[0][FAT12_BOOT_SIG_OFFSET], 0x55);
  ASSERT_MEM_EQ(disk.data[0] + 43, "TEST DISK  ", 11);
  ASSERT_MEM_EQ(disk.data[FAT12_ROOT_START], "TEST DISK  ", 11);
  ASSERT_EQ(disk.data[FAT12_ROOT_START][11], FAT12_ATTR_VOLUME_ID);
  ASSERT_EQ(mount_into(&fat, &disk), DISK_OK);
  ASSERT_EQ(free_clusters(&fat), FAT12_DATA_CLUSTERS);

  vdisk_format_valid(&disk);
  disk.writes_before_failure = 0;
  ASSERT_EQ(cache_init(&cache, vdisk_device(&disk)), DISK_OK);
  ASSERT_EQ(cache_bind(&cache), DISK_OK);
  ASSERT_EQ(fat12_format(&fat, &cache, "TEST", true, NULL, NULL),
            DISK_ERR_VERIFY);
  disk.writes_before_failure = -1;
  progress_calls = 0;
  progress_done = 0;
  ASSERT_EQ(fat12_format(&fat, &cache, NULL, true, progress, NULL), DISK_OK);
  ASSERT_EQ(progress_calls, DISK_TRACK_COUNT);
  ASSERT_MEM_EQ(disk.data[0] + 43, "NO NAME    ", 11);
  ASSERT_EQ(disk.data[FAT12_ROOT_START][0], FAT12_DIRENT_END);
}

int main(void) {
  printf("=== FAT12 Tests ===\n\n");
  RUN_TEST(test_strict_geometry_and_little_endian);
  RUN_TEST(test_init_reads_every_fat_copy);
  RUN_TEST(test_canonical_name_api);
  RUN_TEST(test_open_is_fallible_and_null_safe);
  RUN_TEST(test_create_write_seek_read_delete_rename);
  RUN_TEST(test_directories_are_not_files);
  RUN_TEST(test_typed_read_preserves_partial_progress);
  RUN_TEST(test_sequential_read_reads_each_track_once);
  RUN_TEST(test_read_rejects_cycles_without_replaying_data);
  RUN_TEST(test_typed_write_preserves_partial_progress_and_retries);
  RUN_TEST(test_close_retries_each_commit_phase);
  RUN_TEST(test_close_retries_reclaim_phase_when_replacing);
  RUN_TEST(test_abort_is_safe_only_before_commit);
  RUN_TEST(test_full_disk_abort_never_publishes_fat_metadata);
  RUN_TEST(test_copy_on_write_full_disk_preserves_old_file);
  RUN_TEST(test_allocation_wraps_complete_cluster_space);
  RUN_TEST(test_new_allocation_rejects_referenced_free_cluster);
  RUN_TEST(test_creation_advances_root_end_marker);
  RUN_TEST(test_read_only_entries_are_enforced);
  RUN_TEST(test_mutators_refuse_crosslinked_reclamation);
  RUN_TEST(test_fsck_repairs_lost_clusters_and_fat_copies);
  RUN_TEST(test_fsck_selects_the_consistent_fat_copy);
  RUN_TEST(test_fsck_refuses_ambiguous_fat_copies_without_writes);
  RUN_TEST(test_fsck_preserves_bad_cluster_markers);
  RUN_TEST(test_fsck_repairs_loop_and_short_size);
  RUN_TEST(test_fsck_repairs_file_crosslinks_deterministically);
  RUN_TEST(test_fsck_repairs_crosslink_after_unique_prefix);
  RUN_TEST(test_fsck_repairs_excess_tail_and_zero_size_chain);
  RUN_TEST(test_fsck_traverses_forty_directories);
  RUN_TEST(test_fsck_removes_duplicate_directory_reference);
  RUN_TEST(test_fsck_removes_later_duplicate_name_and_reclaims_chain);
  RUN_TEST(test_duplicate_repair_removes_name_before_reclaim);
  RUN_TEST(test_fsck_multi_action_repair_plan_is_stable);
  RUN_TEST(test_fsck_read_failure_never_mutates);
  RUN_TEST(test_fsck_finishes_directory_reads_before_writes);
  RUN_TEST(test_fsck_repairs_many_duplicates_in_one_pass);
  RUN_TEST(test_fsck_bad_union_survives_duplicate_removal);
  RUN_TEST(test_fsck_repairs_identical_invalid_markers);
  RUN_TEST(test_marker_repair_converges_across_write_failure);
  RUN_TEST(test_verified_track_failure_boundaries_preserve_old_or_new);
  RUN_TEST(test_torn_metadata_track_is_detected_and_repaired);
  RUN_TEST(test_fsck_dry_run_is_repeatable_and_immutable);
  RUN_TEST(test_format_is_typed_and_writes_whole_metadata_tracks);
  RUN_TEST(test_fsck_cuts_directory_chain_before_bad_cluster);
  RUN_TEST(test_fsck_removes_directory_starting_in_bad_cluster);
  RUN_TEST(test_fsck_removes_directory_with_invalid_start_cluster);
  RUN_TEST(test_fsck_cuts_directory_chain_at_invalid_link);
  RUN_TEST(test_fsck_cuts_looping_directory_chain);
  RUN_TEST(test_fsck_cuts_directory_chain_crossing_into_file);
  RUN_TEST(test_fsck_removes_nested_duplicate_directory_reference);
  RUN_TEST(test_fsck_truncates_file_chain_at_invalid_link);
  RUN_TEST(test_fsck_caps_oversized_file_at_disk_length);
  RUN_TEST(test_fsck_clean_flags_every_field);
  RUN_TEST(test_fsck_repair_aborts_when_directory_pass_cannot_flush);
  RUN_TEST(test_fsck_picks_structurally_sound_fat_when_markers_are_invalid);
  RUN_TEST(test_fsck_refuses_fat_copies_reaching_different_clusters);
  RUN_TEST(test_open_rejects_directory_and_volume_entries);
  RUN_TEST(test_open_rejects_damaged_chains);
  RUN_TEST(test_read_and_seek_detect_chain_damaged_after_open);
  RUN_TEST(test_writer_is_busy_after_partial_commit_and_remembers_full_disk);
  RUN_TEST(test_allocation_wraps_around_used_tail);
  RUN_TEST(test_open_write_rejects_damaged_existing_chain);
  RUN_TEST(test_format_rejects_invalid_labels);
  RUN_TEST(test_hardware_scenario_reports_and_repairs_exactly);
  TEST_RESULTS();
}
