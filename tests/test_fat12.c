#include "test.h"
#include "vdisk.h"
#include "../src/fat12.h"

typedef struct {
  vdisk_t disk;
  int reads_before_failure;
  int writes_before_failure;
  block_status_t read_failure;
  block_status_t write_failure;
  bool apply_failed_write;
  bool fail_reads_after_write;
} fault_disk_t;

typedef struct {
  vdisk_t disk;
  int writes_before_tear;
  bool tear_next_write;
} torn_disk_t;

static vdisk_t disk;
static fat12_t fat;
static uint8_t data_a[65536];
static uint8_t data_b[65536];
static uint8_t read_buffer[65536];

static fat12_io_t disk_io(vdisk_t *target) {
  fat12_io_t io = {
    .read = vdisk_read,
    .write = vdisk_write,
    .ctx = target,
  };
  return io;
}

static block_status_t fault_read(
    void *ctx, uint16_t lba, uint8_t out[DISK_SECTOR_SIZE]) {
  fault_disk_t *fault = ctx;
  if (fault->fail_reads_after_write && fault->disk.track_writes != 0) {
    return fault->read_failure;
  }
  if (fault->reads_before_failure == 0) return fault->read_failure;
  if (fault->reads_before_failure > 0) fault->reads_before_failure--;
  return vdisk_read(&fault->disk, lba, out);
}

static block_status_t fault_write(void *ctx, const track_t *track) {
  fault_disk_t *fault = ctx;
  if (fault->writes_before_failure == 0) {
    if (fault->apply_failed_write) {
      block_status_t status = vdisk_write(&fault->disk, track);
      if (status != BLOCK_OK) return status;
    }
    return fault->write_failure;
  }
  if (fault->writes_before_failure > 0) fault->writes_before_failure--;
  return vdisk_write(&fault->disk, track);
}

static fat12_io_t fault_io(fault_disk_t *target) {
  fat12_io_t io = {
    .read = fault_read,
    .write = fault_write,
    .ctx = target,
  };
  return io;
}

static block_status_t torn_read(
    void *ctx, uint16_t lba, uint8_t out[DISK_SECTOR_SIZE]) {
  return vdisk_read(&((torn_disk_t *)ctx)->disk, lba, out);
}

static block_status_t torn_write(void *ctx, const track_t *track) {
  torn_disk_t *torn = ctx;
  if (!torn->tear_next_write) return vdisk_write(&torn->disk, track);
  if (torn->writes_before_tear > 0) {
    torn->writes_before_tear--;
    return vdisk_write(&torn->disk, track);
  }
  torn->tear_next_write = false;
  track_t fragment;
  memset(&fragment, 0, sizeof(fragment));
  fragment.cylinder = track->cylinder;
  fragment.head = track->head;
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    if (!track_has(track, sector)) continue;
    memcpy(fragment.data[sector], track->data[sector], DISK_SECTOR_SIZE);
    track_mark(&fragment, sector);
    break;
  }
  block_status_t status = vdisk_write(&torn->disk, &fragment);
  return status == BLOCK_OK ? BLOCK_ERR_IO : status;
}

static fat12_io_t torn_io(torn_disk_t *target) {
  fat12_io_t io = {
    .read = torn_read,
    .write = torn_write,
    .ctx = target,
  };
  return io;
}

static void fault_init(fault_disk_t *fault) {
  memset(fault, 0, sizeof(*fault));
  vdisk_format_valid(&fault->disk);
  fault->reads_before_failure = -1;
  fault->writes_before_failure = -1;
  fault->read_failure = BLOCK_ERR_TIMEOUT;
  fault->write_failure = BLOCK_ERR_VERIFY;
}

static void mount_clean(void) {
  vdisk_format_valid(&disk);
  ASSERT_EQ(fat12_init(&fat, disk_io(&disk)), FAT12_OK);
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
  ASSERT_EQ(fat12_open_write(filesystem, name, &writer), FAT12_OK);
  size_t offset = 0;
  while (offset < length) {
    fat12_result_t result = fat12_write(
        &writer, data + offset, length - offset);
    offset += result.count;
    ASSERT_EQ(result.error, FAT12_OK);
  }
  ASSERT_EQ(fat12_close_write(&writer), FAT12_OK);
}

static size_t read_file(fat12_t *filesystem, const char *name,
                        uint8_t *out, size_t capacity) {
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(filesystem, name, &entry), FAT12_OK);
  fat12_file_t file;
  ASSERT_EQ(fat12_open(filesystem, &entry, &file), FAT12_OK);
  fat12_result_t result = fat12_read(&file, out, capacity);
  ASSERT_EQ(result.error, FAT12_OK);
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
}

static void raw_dirent(vdisk_t *target, uint16_t index,
                       const char name[static 8], const char ext[static 3],
                       uint8_t attr, uint16_t start, uint32_t size) {
  uint16_t lba = FAT12_RESERVED_SECTORS +
      FAT12_NUM_FATS * FAT12_SECTORS_PER_FAT +
      index * FAT12_DIR_ENTRY_SIZE / DISK_SECTOR_SIZE;
  uint16_t offset = index * FAT12_DIR_ENTRY_SIZE % DISK_SECTOR_SIZE;
  raw_dirent_at(target, lba, offset, name, ext, attr, start, size);
}

static uint16_t entry_next(fat12_t *filesystem, uint16_t cluster) {
  uint16_t next = 0;
  ASSERT_EQ(fat12_get_entry(filesystem, cluster, &next), FAT12_OK);
  return next;
}

static uint16_t free_clusters(fat12_t *filesystem) {
  uint16_t count = 0;
  ASSERT_EQ(fat12_free_count(filesystem, &count), FAT12_OK);
  return count;
}

static fat12_fsck_t repair_to_convergence(fat12_t *filesystem) {
  fat12_fsck_t report;
  for (uint16_t pass = 0; pass < FAT12_ROOT_ENTRIES; pass++) {
    ASSERT_EQ(fat12_fsck(filesystem, &report, true), FAT12_OK);
    ASSERT(!report.incomplete);
    if (!report.repair_pending) return report;
  }
  ASSERT(false);
  memset(&report, 0, sizeof(report));
  return report;
}

TEST(test_strict_geometry_and_little_endian) {
  mount_clean();
  ASSERT_EQ(fat12_last_io(&fat), BLOCK_OK);
  ASSERT_EQ(FAT12_DATA_START, 33);
  ASSERT_EQ(FAT12_DATA_CLUSTERS, 2847);

  static const uint8_t fields[] = {
    11, 13, 14, 16, 17, 19, 21, 22, 24, 26, 28, 32
  };
  for (size_t index = 0; index < sizeof(fields); index++) {
    vdisk_format_valid(&disk);
    disk.data[0][fields[index]] ^= 0x5A;
    ASSERT_EQ(fat12_init(&fat, disk_io(&disk)), FAT12_ERR_INVALID);
  }

  vdisk_format_valid(&disk);
  disk.data[0][11] = 0;
  disk.data[0][12] = 2;
  ASSERT_EQ(fat12_init(&fat, disk_io(&disk)), FAT12_OK);
}

TEST(test_init_reads_every_fat_copy) {
  fault_disk_t fault;
  fault_init(&fault);
  fault.reads_before_failure = 10;
  ASSERT_EQ(fat12_init(&fat, fault_io(&fault)), FAT12_ERR_READ);
  ASSERT_EQ(fat12_last_io(&fat), BLOCK_ERR_TIMEOUT);

  fault_init(&fault);
  fault.disk.data[FAT12_FAT2_START + 8][17] ^= 0x80;
  ASSERT_EQ(fat12_init(&fat, fault_io(&fault)), FAT12_OK);
  ASSERT(fat.fat_mismatch);
}

TEST(test_canonical_name_api) {
  fat12_name_t name;
  ASSERT_EQ(fat12_name_parse("alpha.bin", &name), FAT12_OK);
  ASSERT_MEM_EQ(name.name, "ALPHA   ", 8);
  ASSERT_MEM_EQ(name.ext, "BIN", 3);
  ASSERT_EQ(fat12_name_parse("TOO-LONG-NAME.BIN", &name), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_name_parse("A.B.C", &name), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_name_parse("BAD NAME", &name), FAT12_ERR_INVALID);
}

TEST(test_open_is_fallible_and_null_safe) {
  mount_clean();
  fat12_dirent_t entry;
  memset(&entry, 0, sizeof(entry));
  memset(entry.name, ' ', sizeof(entry.name));
  memset(entry.ext, ' ', sizeof(entry.ext));
  entry.attr = FAT12_ATTR_ARCHIVE;
  fat12_file_t file;
  ASSERT_EQ(fat12_open(NULL, &entry, &file), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_open(&fat, NULL, &file), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_open(&fat, &entry, NULL), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_open(&fat, &entry, &file), FAT12_OK);
}

TEST(test_create_write_seek_read_delete_rename) {
  mount_clean();
  fill_pattern(data_a, 7000, 1);
  write_file(&fat, "FIRST.BIN", data_a, 7000);
  ASSERT_EQ(read_file(&fat, "FIRST.BIN", read_buffer, sizeof(read_buffer)), 7000);
  ASSERT_MEM_EQ(read_buffer, data_a, 7000);

  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "FIRST.BIN", &entry), FAT12_OK);
  fat12_file_t file;
  ASSERT_EQ(fat12_open(&fat, &entry, &file), FAT12_OK);
  ASSERT_EQ(fat12_seek(&file, 513), FAT12_OK);
  fat12_result_t result = fat12_read(&file, read_buffer, 1000);
  ASSERT_EQ(result.error, FAT12_OK);
  ASSERT_EQ(result.count, 1000);
  ASSERT_MEM_EQ(read_buffer, data_a + 513, 1000);

  ASSERT_EQ(fat12_rename(&fat, "FIRST.BIN", "SECOND.BIN"), FAT12_OK);
  ASSERT_EQ(fat12_find(&fat, "FIRST.BIN", &entry), FAT12_ERR_NOT_FOUND);
  ASSERT_EQ(fat12_delete(&fat, "SECOND.BIN"), FAT12_OK);
  ASSERT_EQ(fat12_find(&fat, "SECOND.BIN", &entry), FAT12_ERR_NOT_FOUND);
  ASSERT_EQ(free_clusters(&fat), FAT12_DATA_CLUSTERS);
}

TEST(test_typed_read_preserves_partial_progress) {
  fault_disk_t fault;
  fault_init(&fault);
  ASSERT_EQ(fat12_init(&fat, fault_io(&fault)), FAT12_OK);
  fill_pattern(data_a, 1400, 2);
  write_file(&fat, "READ.BIN", data_a, 1400);

  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "READ.BIN", &entry), FAT12_OK);
  fat12_file_t file;
  ASSERT_EQ(fat12_open(&fat, &entry, &file), FAT12_OK);
  fault.reads_before_failure = 4;
  fat12_result_t result = fat12_read(&file, read_buffer, 1400);
  ASSERT_EQ(result.error, FAT12_ERR_READ);
  ASSERT_EQ(result.count, 1024);
  ASSERT_MEM_EQ(read_buffer, data_a, result.count);
  ASSERT_EQ(fat12_last_io(&fat), BLOCK_ERR_TIMEOUT);

  fault.reads_before_failure = -1;
  result = fat12_read(&file, read_buffer + 1024, 376);
  ASSERT_EQ(result.error, FAT12_OK);
  ASSERT_EQ(result.count, 376);
  ASSERT_MEM_EQ(read_buffer, data_a, 1400);
}

TEST(test_sequential_read_has_linear_fat_io) {
  mount_clean();
  fill_pattern(data_a, sizeof(data_a), 21);
  int writes = disk.track_writes;
  write_file(&fat, "LINEAR.BIN", data_a, sizeof(data_a));
  ASSERT(disk.track_writes - writes <= 12);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "LINEAR.BIN", &entry), FAT12_OK);
  fat12_file_t file;
  disk.read_count = 0;
  ASSERT_EQ(fat12_open(&fat, &entry, &file), FAT12_OK);
  size_t clusters = sizeof(data_a) / DISK_SECTOR_SIZE;
  ASSERT((size_t)disk.read_count <= clusters * 2u);
  disk.read_count = 0;
  fat12_result_t result = fat12_read(&file, read_buffer, sizeof(read_buffer));
  ASSERT_EQ(result.error, FAT12_OK);
  ASSERT_EQ(result.count, sizeof(data_a));
  ASSERT((size_t)disk.read_count <= clusters * 3u);
  ASSERT_MEM_EQ(read_buffer, data_a, sizeof(data_a));
}

TEST(test_read_rejects_cycles_without_replaying_data) {
  mount_clean();
  vdisk_set_fat_entry(&disk, 2, 3);
  vdisk_set_fat_entry(&disk, 3, 2);
  raw_dirent(&disk, 0, "CYCLE   ", "BIN", FAT12_ATTR_ARCHIVE, 2, 1536);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "CYCLE.BIN", &entry), FAT12_OK);
  fat12_file_t file;
  ASSERT_EQ(fat12_open(&fat, &entry, &file), FAT12_ERR_CORRUPT);
}

TEST(test_typed_write_preserves_partial_progress_and_retries) {
  fault_disk_t fault;
  fault_init(&fault);
  ASSERT_EQ(fat12_init(&fat, fault_io(&fault)), FAT12_OK);
  fill_pattern(data_a, 50000, 3);

  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "RETRY.BIN", &writer), FAT12_OK);
  fault.writes_before_failure = 0;
  fat12_result_t first = fat12_write(&writer, data_a, 50000);
  ASSERT_EQ(first.error, FAT12_ERR_WRITE);
  ASSERT(first.count > 0);
  ASSERT(first.count < 50000);
  ASSERT_EQ(fat12_last_io(&fat), BLOCK_ERR_VERIFY);

  fault.writes_before_failure = -1;
  fat12_result_t second = fat12_write(
      &writer, data_a + first.count, 50000 - first.count);
  ASSERT_EQ(second.error, FAT12_OK);
  ASSERT_EQ(first.count + second.count, 50000);
  ASSERT_EQ(fat12_close_write(&writer), FAT12_OK);
  ASSERT_EQ(read_file(&fat, "RETRY.BIN", read_buffer, sizeof(read_buffer)), 50000);
  ASSERT_MEM_EQ(read_buffer, data_a, 50000);
}

TEST(test_close_retries_each_commit_phase) {
  for (int failure = 0; failure < 4; failure++) {
    fault_disk_t fault;
    fault_init(&fault);
    ASSERT_EQ(fat12_init(&fat, fault_io(&fault)), FAT12_OK);
    fill_pattern(data_a, 900, (uint32_t)failure + 4u);
    fat12_writer_t writer;
    ASSERT_EQ(fat12_open_write(&fat, "PHASE.BIN", &writer), FAT12_OK);
    fat12_result_t written = fat12_write(&writer, data_a, 900);
    ASSERT_EQ(written.error, FAT12_OK);
    ASSERT_EQ(written.count, 900);

    fault.writes_before_failure = failure;
    fat12_err_t result = fat12_close_write(&writer);
    if (result == FAT12_OK) continue;
    ASSERT_EQ(result, FAT12_ERR_WRITE);
    ASSERT(fat12_busy(&fat));
    ASSERT_EQ(fat12_abort_write(&writer), FAT12_ERR_BUSY);
    fault.writes_before_failure = -1;
    ASSERT_EQ(fat12_close_write(&writer), FAT12_OK);
    ASSERT(!fat12_busy(&fat));
    ASSERT_EQ(read_file(&fat, "PHASE.BIN", read_buffer, sizeof(read_buffer)), 900);
    ASSERT_MEM_EQ(read_buffer, data_a, 900);
  }
}

TEST(test_abort_is_safe_only_before_commit) {
  mount_clean();
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "DROP.BIN", &writer), FAT12_OK);
  fill_pattern(data_a, 40000, 8);
  fat12_result_t result = fat12_write(&writer, data_a, 40000);
  ASSERT_EQ(result.error, FAT12_OK);
  ASSERT_EQ(fat12_abort_write(&writer), FAT12_OK);
  ASSERT(!fat12_busy(&fat));
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "DROP.BIN", &entry), FAT12_ERR_NOT_FOUND);
  ASSERT_EQ(free_clusters(&fat), FAT12_DATA_CLUSTERS);

  ASSERT_EQ(fat12_open_write(&fat, "MEDIA.BIN", &writer), FAT12_OK);
  result = fat12_write(&writer, data_a, 1000);
  ASSERT_EQ(result.error, FAT12_OK);
  fat12_forget_write(&writer);
  ASSERT(!fat12_busy(&fat));
  ASSERT_NULL(writer.fat);
  ASSERT_EQ(fat12_find(&fat, "MEDIA.BIN", &entry), FAT12_ERR_NOT_FOUND);
}

TEST(test_full_disk_abort_never_publishes_fat_metadata) {
  mount_clean();
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "ABORTALL.BIN", &writer), FAT12_OK);
  uint8_t cluster[DISK_SECTOR_SIZE];
  memset(cluster, 0xC3, sizeof(cluster));
  int writes = disk.track_writes;
  for (uint16_t index = 0; index < FAT12_DATA_CLUSTERS; index++) {
    fat12_result_t result = fat12_write(&writer, cluster, sizeof(cluster));
    ASSERT_EQ(result.error, FAT12_OK);
    ASSERT_EQ(result.count, sizeof(cluster));
  }
  ASSERT((unsigned)(disk.track_writes - writes) <= DISK_TRACK_COUNT);
  ASSERT_EQ(fat12_abort_write(&writer), FAT12_OK);
  ASSERT_EQ(free_clusters(&fat), FAT12_DATA_CLUSTERS);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "ABORTALL.BIN", &entry), FAT12_ERR_NOT_FOUND);
}

TEST(test_copy_on_write_full_disk_preserves_old_file) {
  mount_clean();
  fill_pattern(data_a, 1024, 9);
  fill_pattern(data_b, 1024, 10);
  write_file(&fat, "TARGET.BIN", data_a, 1024);

  static uint8_t cluster[DISK_SECTOR_SIZE];
  memset(cluster, 0xA5, sizeof(cluster));
  fat12_writer_t filler;
  ASSERT_EQ(fat12_open_write(&fat, "FILL.BIN", &filler), FAT12_OK);
  uint16_t available = free_clusters(&fat);
  for (uint16_t index = 0; index < available; index++) {
    fat12_result_t result = fat12_write(&filler, cluster, sizeof(cluster));
    ASSERT_EQ(result.error, FAT12_OK);
    ASSERT_EQ(result.count, sizeof(cluster));
  }
  ASSERT_EQ(fat12_close_write(&filler), FAT12_OK);
  ASSERT_EQ(free_clusters(&fat), 0);

  fat12_writer_t replacement;
  ASSERT_EQ(fat12_open_write(&fat, "TARGET.BIN", &replacement), FAT12_OK);
  fat12_result_t result = fat12_write(&replacement, data_b, 1024);
  ASSERT_EQ(result.error, FAT12_ERR_FULL);
  ASSERT_EQ(result.count, 0);
  ASSERT_EQ(fat12_abort_write(&replacement), FAT12_OK);
  ASSERT_EQ(read_file(&fat, "TARGET.BIN", read_buffer, sizeof(read_buffer)), 1024);
  ASSERT_MEM_EQ(read_buffer, data_a, 1024);
}

TEST(test_allocation_wraps_complete_cluster_space) {
  mount_clean();
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "WRAP.BIN", &writer), FAT12_OK);
  uint16_t last = FAT12_CLUSTER_LIMIT - 1u;
  vdisk_set_fat_entry(&disk, last, 0xFFF);
  writer.first_cluster = last;
  writer.prev_cluster = last;
  writer.bytes_written = DISK_SECTOR_SIZE;
  fat12_result_t result = fat12_write(&writer, (const uint8_t *)"x", 1);
  ASSERT_EQ(result.error, FAT12_OK);
  ASSERT_EQ(result.count, 1);
  ASSERT_EQ(fat12_close_write(&writer), FAT12_OK);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "WRAP.BIN", &entry), FAT12_OK);
  ASSERT_EQ(entry.start_cluster, last);
  ASSERT_EQ(entry_next(&fat, last), 2);
}

TEST(test_new_allocation_rejects_referenced_free_cluster) {
  mount_clean();
  raw_dirent(&disk, 0, "LIVE    ", "BIN", FAT12_ATTR_ARCHIVE,
             2, DISK_SECTOR_SIZE);
  memset(disk.data[FAT12_DATA_START], 0x6D, DISK_SECTOR_SIZE);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 2), 0);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "NEW.BIN", &writer), FAT12_ERR_CORRUPT);
  ASSERT_EQ(disk.data[FAT12_DATA_START][0], 0x6D);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 2), 0);
}

TEST(test_creation_advances_root_end_marker) {
  mount_clean();
  vdisk_set_fat_entry(&disk, 200, 0xFFF);
  raw_dirent(&disk, 1, "GHOST   ", "BIN", FAT12_ATTR_ARCHIVE,
             200, DISK_SECTOR_SIZE);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "NEW.BIN", &writer), FAT12_OK);
  ASSERT_EQ(fat12_close_write(&writer), FAT12_OK);
  uint16_t root = FAT12_FAT2_START + FAT12_SECTORS_PER_FAT;
  ASSERT_EQ(disk.data[root][FAT12_DIR_ENTRY_SIZE], FAT12_DIRENT_END);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "NEW.BIN", &entry), FAT12_OK);
  ASSERT_EQ(fat12_find(&fat, "GHOST.BIN", &entry), FAT12_ERR_NOT_FOUND);
}

TEST(test_read_only_entries_are_enforced) {
  mount_clean();
  raw_dirent(&disk, 0, "LOCKED  ", "BIN",
             FAT12_ATTR_ARCHIVE | FAT12_ATTR_READ_ONLY, 0, 0);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "LOCKED.BIN", &writer), FAT12_ERR_READ_ONLY);
  ASSERT_EQ(fat12_delete(&fat, "LOCKED.BIN"), FAT12_ERR_READ_ONLY);
  ASSERT_EQ(fat12_rename(&fat, "LOCKED.BIN", "OTHER.BIN"), FAT12_ERR_READ_ONLY);
}

TEST(test_mutators_refuse_crosslinked_reclamation) {
  mount_clean();
  fill_pattern(data_a, 1024, 20);
  write_file(&fat, "FIRST.BIN", data_a, 1024);
  fat12_dirent_t first;
  ASSERT_EQ(fat12_find(&fat, "FIRST.BIN", &first), FAT12_OK);
  raw_dirent(&disk, 1, "SECOND  ", "BIN", FAT12_ATTR_ARCHIVE,
             first.start_cluster, first.size);

  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "FIRST.BIN", &writer), FAT12_ERR_CORRUPT);
  ASSERT_EQ(fat12_delete(&fat, "FIRST.BIN"), FAT12_ERR_CORRUPT);
  ASSERT_EQ(read_file(&fat, "FIRST.BIN", read_buffer, sizeof(read_buffer)), 1024);
  ASSERT_MEM_EQ(read_buffer, data_a, 1024);
  ASSERT_EQ(read_file(&fat, "SECOND.BIN", read_buffer, sizeof(read_buffer)), 1024);
  ASSERT_MEM_EQ(read_buffer, data_a, 1024);

  mount_clean();
  write_file(&fat, "FIRST.BIN", data_a, 1024);
  ASSERT_EQ(fat12_find(&fat, "FIRST.BIN", &first), FAT12_OK);
  vdisk_set_fat_entry(&disk, 100, 0xFFF);
  raw_dirent(&disk, 1, "SUBDIR  ", "   ", FAT12_ATTR_DIRECTORY, 100, 0);
  memset(disk.data[FAT12_DATA_START + 98], 0, DISK_SECTOR_SIZE);
  raw_dirent_at(&disk, FAT12_DATA_START + 98, 0,
                "INNER   ", "BIN", FAT12_ATTR_ARCHIVE,
                first.start_cluster, first.size);
  ASSERT_EQ(fat12_open_write(&fat, "FIRST.BIN", &writer), FAT12_ERR_CORRUPT);
  ASSERT_EQ(fat12_delete(&fat, "FIRST.BIN"), FAT12_ERR_CORRUPT);
  ASSERT_EQ(read_file(&fat, "FIRST.BIN", read_buffer, sizeof(read_buffer)), 1024);
  ASSERT_MEM_EQ(read_buffer, data_a, 1024);
}

TEST(test_fsck_repairs_lost_clusters_and_fat_copies) {
  mount_clean();
  vdisk_set_fat_entry(&disk, 100, 101);
  vdisk_set_fat_entry(&disk, 101, 0xFFF);
  vdisk_set_fat_entry(&disk, 700, 0xFFF);
  disk.data[FAT12_FAT2_START + 8][400] ^= 0x42;

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.lost_clusters, 3);
  ASSERT(report.fat_mismatch);
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT_EQ(report.freed, 3);
  ASSERT(report.repaired_fat2);
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.lost_clusters, 0);
  ASSERT(!report.fat_mismatch);
}

TEST(test_fsck_selects_the_consistent_fat_copy) {
  for (uint8_t damaged = 0; damaged < 2; damaged++) {
    mount_clean();
    fill_pattern(data_a, 1536, 22u + damaged);
    write_file(&fat, "MIRROR.BIN", data_a, 1536);
    fat12_dirent_t entry;
    ASSERT_EQ(fat12_find(&fat, "MIRROR.BIN", &entry), FAT12_OK);
    uint16_t damaged_start = damaged == 0 ? FAT12_FAT1_START : FAT12_FAT2_START;
    vdisk_set_fat_copy_entry(&disk, damaged_start, entry.start_cluster, 0xFFF);
    ASSERT_EQ(fat12_init(&fat, disk_io(&disk)), FAT12_OK);
    ASSERT(fat.fat_mismatch);
    ASSERT_EQ(fat.fat_start,
              damaged == 0 ? FAT12_FAT2_START : FAT12_FAT1_START);
    ASSERT_EQ(read_file(&fat, "MIRROR.BIN", read_buffer,
                        sizeof(read_buffer)), 1536);
    ASSERT_MEM_EQ(read_buffer, data_a, 1536);

    fat12_fsck_t report;
    ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
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
  ASSERT_EQ(fat12_find(&fat, "CHOICE.BIN", &entry), FAT12_OK);
  uint16_t second = entry_next(&fat, entry.start_cluster);
  uint16_t alternate = second + 1u;
  vdisk_set_fat_copy_entry(
      &disk, FAT12_FAT2_START, entry.start_cluster, alternate);
  vdisk_set_fat_copy_entry(&disk, FAT12_FAT2_START, second, 0);
  vdisk_set_fat_copy_entry(&disk, FAT12_FAT2_START, alternate, 0xFFF);
  memcpy(data_b, disk.data[FAT12_FAT1_START],
         FAT12_NUM_FATS * FAT12_SECTORS_PER_FAT * DISK_SECTOR_SIZE);
  int writes = disk.track_writes;

  fat12_t candidate;
  ASSERT_EQ(fat12_init(&candidate, disk_io(&disk)), FAT12_ERR_AMBIGUOUS);
  ASSERT_EQ(disk.track_writes, writes);

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_ERR_AMBIGUOUS);
  ASSERT(report.fat_ambiguous);
  ASSERT_EQ(report.authoritative_fat, 0);
  ASSERT_EQ(disk.track_writes, writes);
  ASSERT_MEM_EQ(data_b, disk.data[FAT12_FAT1_START],
                FAT12_NUM_FATS * FAT12_SECTORS_PER_FAT * DISK_SECTOR_SIZE);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "BLOCKED.BIN", &writer),
            FAT12_ERR_AMBIGUOUS);
  ASSERT_EQ(disk.track_writes, writes);
}

TEST(test_fsck_preserves_bad_cluster_markers) {
  mount_clean();
  vdisk_set_fat_entry(&disk, 100, 0xFF7);
  raw_dirent(&disk, 0, "BADLAST ", "BIN", FAT12_ATTR_ARCHIVE,
             100, DISK_SECTOR_SIZE);
  vdisk_set_fat_entry(&disk, 110, 111);
  vdisk_set_fat_entry(&disk, 111, 0xFF7);
  raw_dirent(&disk, 1, "BADMID  ", "BIN", FAT12_ATTR_ARCHIVE,
             110, DISK_SECTOR_SIZE * 2u);

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 100), 0xFF7);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT2_START, 100), 0xFF7);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 111), 0xFF7);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT2_START, 111), 0xFF7);
  ASSERT(fat12_is_eof(vdisk_get_fat_copy_entry(
      &disk, FAT12_FAT1_START, 110)));
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "BADLAST.BIN", &entry), FAT12_OK);
  ASSERT_EQ(entry.start_cluster, 0);
  ASSERT_EQ(entry.size, 0);
  ASSERT_EQ(fat12_find(&fat, "BADMID.BIN", &entry), FAT12_OK);
  ASSERT_EQ(entry.start_cluster, 110);
  ASSERT_EQ(entry.size, DISK_SECTOR_SIZE);
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.broken_chains, 0);
  ASSERT_EQ(report.size_mismatches, 0);
}

TEST(test_fsck_repairs_loop_and_short_size) {
  mount_clean();
  fill_pattern(data_a, 1536, 11);
  write_file(&fat, "LOOP.BIN", data_a, 1536);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "LOOP.BIN", &entry), FAT12_OK);
  uint16_t second = entry_next(&fat, entry.start_cluster);
  uint16_t third = entry_next(&fat, second);
  vdisk_set_fat_entry(&disk, third, second);

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.loops, 1);
  ASSERT(report.broken_chains > 0);
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.loops, 0);
  ASSERT_EQ(report.broken_chains, 0);

  vdisk_set_fat_entry(&disk, entry.start_cluster, 0xFFF);
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT(report.truncated_files > 0);
  ASSERT_EQ(fat12_find(&fat, "LOOP.BIN", &entry), FAT12_OK);
  ASSERT_EQ(entry.size, DISK_SECTOR_SIZE);
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.size_mismatches, 0);
  ASSERT_EQ(report.lost_clusters, 0);
}

TEST(test_fsck_repairs_file_crosslinks_deterministically) {
  mount_clean();
  fill_pattern(data_a, 1024, 12);
  write_file(&fat, "FIRST.BIN", data_a, 1024);
  fat12_dirent_t first;
  ASSERT_EQ(fat12_find(&fat, "FIRST.BIN", &first), FAT12_OK);
  raw_dirent(&disk, 1, "SECOND  ", "BIN", FAT12_ATTR_ARCHIVE,
             first.start_cluster, first.size);

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.crosslinked, 1);
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.crosslinked, 0);
  fat12_dirent_t second;
  ASSERT_EQ(fat12_find(&fat, "SECOND.BIN", &second), FAT12_OK);
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
  ASSERT_EQ(fat12_find(&fat, "FIRST.BIN", &first), FAT12_OK);
  uint16_t shared = entry_next(&fat, first.start_cluster);
  vdisk_set_fat_entry(&disk, 200, shared);
  raw_dirent(&disk, 1, "SECOND  ", "BIN", FAT12_ATTR_ARCHIVE, 200, 1024);

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.crosslinked, 0);
  ASSERT_EQ(report.size_mismatches, 0);
  fat12_dirent_t second;
  ASSERT_EQ(fat12_find(&fat, "SECOND.BIN", &second), FAT12_OK);
  ASSERT_EQ(second.start_cluster, 200);
  ASSERT_EQ(second.size, DISK_SECTOR_SIZE);
  ASSERT(fat12_is_eof(entry_next(&fat, 200)));
}

TEST(test_fsck_repairs_excess_tail_and_zero_size_chain) {
  mount_clean();
  fill_pattern(data_a, 512, 14);
  write_file(&fat, "TAIL.BIN", data_a, 512);
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "TAIL.BIN", &entry), FAT12_OK);
  vdisk_set_fat_entry(&disk, entry.start_cluster, 300);
  vdisk_set_fat_entry(&disk, 300, 0xFFF);
  vdisk_set_fat_entry(&disk, 400, 0xFFF);
  raw_dirent(&disk, 1, "ZERO    ", "BIN", FAT12_ATTR_ARCHIVE, 400, 0);
  vdisk_set_fat_entry(&disk, 500, 0xFFF);
  raw_dirent(&disk, 2, "HUGE    ", "BIN", FAT12_ATTR_ARCHIVE,
             500, UINT32_MAX);

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT(report.freed >= 2);
  ASSERT(report.freed_tails > 0);
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.lost_clusters, 0);
  ASSERT_EQ(report.size_mismatches, 0);
  fat12_dirent_t zero;
  ASSERT_EQ(fat12_find(&fat, "ZERO.BIN", &zero), FAT12_OK);
  ASSERT_EQ(zero.start_cluster, 0);
  fat12_dirent_t huge;
  ASSERT_EQ(fat12_find(&fat, "HUGE.BIN", &huge), FAT12_OK);
  ASSERT_EQ(huge.size, DISK_SECTOR_SIZE);
}

TEST(test_fsck_traverses_more_than_sixteen_directories) {
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
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.directories, 40);
  ASSERT_EQ(report.lost_clusters, 0);
  ASSERT(!report.incomplete);
}

TEST(test_fsck_removes_duplicate_directory_reference) {
  mount_clean();
  vdisk_set_fat_entry(&disk, 2, 0xFFF);
  memset(disk.data[FAT12_DATA_START], 0, DISK_SECTOR_SIZE);
  raw_dirent(&disk, 0, "FIRST   ", "   ", FAT12_ATTR_DIRECTORY, 2, 0);
  raw_dirent(&disk, 1, "SECOND  ", "   ", FAT12_ATTR_DIRECTORY, 2, 0);
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT_EQ(report.removed_directories, 1);
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.crosslinked, 0);
  ASSERT_EQ(report.directories, 1);
}

TEST(test_fsck_removes_later_duplicate_name_and_reclaims_chain) {
  mount_clean();
  fill_pattern(data_a, DISK_SECTOR_SIZE, 25);
  write_file(&fat, "SAME.BIN", data_a, DISK_SECTOR_SIZE);
  vdisk_set_fat_entry(&disk, 200, 0xFFF);
  memset(disk.data[FAT12_DATA_START + 198], 0xB7, DISK_SECTOR_SIZE);
  raw_dirent(&disk, 1, "SAME    ", "BIN", FAT12_ATTR_ARCHIVE,
             200, DISK_SECTOR_SIZE);

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.duplicate_names, 1);
  ASSERT_EQ(report.lost_clusters, 0);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "OTHER.BIN", &writer), FAT12_ERR_CORRUPT);

  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT_EQ(report.duplicate_names, 1);
  ASSERT_EQ(report.removed_duplicates, 1);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 200), 0);
  uint16_t root = FAT12_FAT2_START + FAT12_SECTORS_PER_FAT;
  ASSERT_EQ(disk.data[root][FAT12_DIR_ENTRY_SIZE], FAT12_DIRENT_FREE);
  ASSERT_EQ(read_file(&fat, "SAME.BIN", read_buffer,
                      sizeof(read_buffer)), DISK_SECTOR_SIZE);
  ASSERT_MEM_EQ(read_buffer, data_a, DISK_SECTOR_SIZE);
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.duplicate_names, 0);
  ASSERT_EQ(report.lost_clusters, 0);
}

TEST(test_duplicate_repair_removes_name_before_reclaim) {
  fault_disk_t fault;
  fault_init(&fault);
  ASSERT_EQ(fat12_init(&fat, fault_io(&fault)), FAT12_OK);
  fill_pattern(data_a, DISK_SECTOR_SIZE, 26);
  write_file(&fat, "SAME.BIN", data_a, DISK_SECTOR_SIZE);
  vdisk_set_fat_entry(&fault.disk, 200, 0xFFF);
  raw_dirent(&fault.disk, 1, "SAME    ", "BIN", FAT12_ATTR_ARCHIVE,
             200, DISK_SECTOR_SIZE);
  fault.writes_before_failure = 1;

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_ERR_WRITE);
  uint16_t root = FAT12_FAT2_START + FAT12_SECTORS_PER_FAT;
  ASSERT_EQ(fault.disk.data[root][FAT12_DIR_ENTRY_SIZE], FAT12_DIRENT_FREE);
  ASSERT(fat12_is_eof(vdisk_get_fat_copy_entry(
      &fault.disk, FAT12_FAT1_START, 200)));

  ASSERT_EQ(fat12_init(&fat, disk_io(&fault.disk)), FAT12_OK);
  ASSERT_EQ(read_file(&fat, "SAME.BIN", read_buffer,
                      sizeof(read_buffer)), DISK_SECTOR_SIZE);
  ASSERT_MEM_EQ(read_buffer, data_a, DISK_SECTOR_SIZE);
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT_EQ(vdisk_get_fat_copy_entry(
      &fault.disk, FAT12_FAT1_START, 200), 0);
}

TEST(test_fsck_multi_action_repair_plan_is_stable) {
  mount_clean();
  fill_pattern(data_a, 2000, 15);
  write_file(&fat, "BROKEN.BIN", data_a, 2000);
  for (uint16_t cluster = 50; cluster < 900; cluster += 73) {
    vdisk_set_fat_entry(&disk, cluster, 0xFFF);
  }
  fat12_dirent_t entry;
  ASSERT_EQ(fat12_find(&fat, "BROKEN.BIN", &entry), FAT12_OK);
  vdisk_set_fat_entry(&disk, entry.start_cluster, 0xFFF);

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT(report.freed > 5);
  ASSERT(report.truncated_files > 0);
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.lost_clusters, 0);
  ASSERT_EQ(report.broken_chains, 0);
  ASSERT_EQ(report.size_mismatches, 0);
  ASSERT(!report.fat_mismatch);
}

TEST(test_fsck_read_failure_never_mutates) {
  fault_disk_t fault;
  fault_init(&fault);
  vdisk_set_fat_entry(&fault.disk, 100, 0xFFF);
  ASSERT_EQ(fat12_init(&fat, fault_io(&fault)), FAT12_OK);
  int writes = fault.disk.track_writes;
  fault.reads_before_failure = 5;
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_ERR_READ);
  ASSERT_EQ(fault.disk.track_writes, writes);
  ASSERT_EQ(fat12_last_io(&fat), BLOCK_ERR_TIMEOUT);
}

TEST(test_fsck_finishes_directory_reads_before_writes) {
  fault_disk_t fault;
  fault_init(&fault);
  vdisk_set_fat_entry(&fault.disk, 2, 0xFFF);
  vdisk_set_fat_entry(&fault.disk, 100, 0xFFF);
  raw_dirent(&fault.disk, 0, "DIRONE  ", "   ", FAT12_ATTR_DIRECTORY, 2, 0);
  raw_dirent(&fault.disk, 1, "DIRTWO  ", "   ", FAT12_ATTR_DIRECTORY, 100, 0);
  uint16_t first = FAT12_DATA_START;
  uint16_t second = FAT12_DATA_START + 98u;
  raw_dirent_at(&fault.disk, first, 0, "SAME    ", "BIN",
                FAT12_ATTR_ARCHIVE, 0, 0);
  raw_dirent_at(&fault.disk, first, FAT12_DIR_ENTRY_SIZE, "SAME    ", "BIN",
                FAT12_ATTR_ARCHIVE, 0, 0);
  raw_dirent_at(&fault.disk, second, 0, "SAME    ", "BIN",
                FAT12_ATTR_ARCHIVE, 0, 0);
  raw_dirent_at(&fault.disk, second, FAT12_DIR_ENTRY_SIZE, "SAME    ", "BIN",
                FAT12_ATTR_ARCHIVE, 0, 0);
  ASSERT_EQ(fat12_init(&fat, fault_io(&fault)), FAT12_OK);
  fault.fail_reads_after_write = true;

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT(!report.repair_pending);
  ASSERT_EQ(report.removed_duplicates, 2);
  ASSERT_EQ(fault.disk.data[first][FAT12_DIR_ENTRY_SIZE], FAT12_DIRENT_FREE);
  ASSERT_EQ(fault.disk.data[second][FAT12_DIR_ENTRY_SIZE], FAT12_DIRENT_FREE);
  ASSERT_EQ(fault.disk.track_writes, 2);

  fault.fail_reads_after_write = false;
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.duplicate_names, 0);
  ASSERT_EQ(report.lost_clusters, 0);
}

TEST(test_fsck_namespace_plan_converges_when_full) {
  mount_clean();
  enum { DIRECTORY_COUNT = FAT12_WRITE_BATCH_MAX + 2u };
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

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT(report.repair_pending);
  ASSERT_EQ(report.removed_duplicates, FAT12_WRITE_BATCH_MAX);
  repair_to_convergence(&fat);
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.duplicate_names, 0);
  ASSERT_EQ(report.lost_clusters, 0);
  for (uint16_t index = 0; index < DIRECTORY_COUNT; index++) {
    uint16_t lba = (uint16_t)(FAT12_DATA_START + index);
    ASSERT_EQ(disk.data[lba][FAT12_DIR_ENTRY_SIZE], FAT12_DIRENT_FREE);
  }
}

TEST(test_fsck_bad_union_survives_duplicate_removal) {
  mount_clean();
  vdisk_set_fat_entry(&disk, 2, 0xFFF);
  vdisk_set_fat_entry(&disk, 200, 0xFFF);
  vdisk_set_fat_copy_entry(&disk, FAT12_FAT2_START, 200, 0xFF7);
  raw_dirent(&disk, 0, "SAME    ", "BIN", FAT12_ATTR_ARCHIVE,
             2, DISK_SECTOR_SIZE);
  raw_dirent(&disk, 1, "SAME    ", "BIN", FAT12_ATTR_ARCHIVE,
             200, DISK_SECTOR_SIZE);

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT_EQ(report.removed_duplicates, 1);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 200), 0xFF7);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT2_START, 200), 0xFF7);
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.duplicate_names, 0);
  ASSERT_EQ(report.lost_clusters, 0);
}

TEST(test_fsck_repairs_identical_invalid_markers) {
  mount_clean();
  disk.data[FAT12_FAT1_START][0] = 0;
  disk.data[FAT12_FAT2_START][0] = 0;
  fat12_t candidate;
  ASSERT_EQ(fat12_init(&candidate, disk_io(&disk)), FAT12_OK);
  ASSERT(candidate.fat_markers_invalid);

  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&candidate, &report, false), FAT12_OK);
  ASSERT(report.fat_markers_invalid);
  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&candidate, "BLOCKED.BIN", &writer),
            FAT12_ERR_CORRUPT);
  ASSERT_EQ(fat12_fsck(&candidate, &report, true), FAT12_OK);
  ASSERT(report.repaired_fat1);
  ASSERT(report.repaired_fat2);
  ASSERT_EQ(disk.data[FAT12_FAT1_START][0], FAT12_MEDIA_DESCRIPTOR);
  ASSERT_EQ(disk.data[FAT12_FAT2_START][0], FAT12_MEDIA_DESCRIPTOR);
  ASSERT_EQ(fat12_init(&candidate, disk_io(&disk)), FAT12_OK);
  ASSERT(!candidate.fat_markers_invalid);
}

TEST(test_marker_repair_converges_across_write_failure) {
  for (uint8_t apply = 0; apply < 2; apply++) {
    fault_disk_t fault;
    fault_init(&fault);
    fault.disk.data[FAT12_FAT1_START][0] = 0;
    fault.disk.data[FAT12_FAT2_START][0] = 0;
    ASSERT_EQ(fat12_init(&fat, fault_io(&fault)), FAT12_OK);
    fault.writes_before_failure = 0;
    fault.apply_failed_write = apply != 0;
    fat12_fsck_t report;
    ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_ERR_WRITE);

    ASSERT_EQ(fat12_init(&fat, disk_io(&fault.disk)), FAT12_OK);
    repair_to_convergence(&fat);
    ASSERT_EQ(fat12_init(&fat, disk_io(&fault.disk)), FAT12_OK);
    ASSERT(!fat.fat_markers_invalid);
    ASSERT_EQ(fault.disk.data[FAT12_FAT1_START][0], FAT12_MEDIA_DESCRIPTOR);
    ASSERT_EQ(fault.disk.data[FAT12_FAT2_START][0], FAT12_MEDIA_DESCRIPTOR);
  }

  torn_disk_t torn;
  memset(&torn, 0, sizeof(torn));
  vdisk_format_valid(&torn.disk);
  torn.disk.data[FAT12_FAT1_START][0] = 0;
  torn.disk.data[FAT12_FAT2_START][0] = 0;
  ASSERT_EQ(fat12_init(&fat, torn_io(&torn)), FAT12_OK);
  torn.tear_next_write = true;
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_ERR_WRITE);
  ASSERT_EQ(fat12_init(&fat, disk_io(&torn.disk)), FAT12_OK);
  repair_to_convergence(&fat);
  ASSERT_EQ(fat12_init(&fat, disk_io(&torn.disk)), FAT12_OK);
  ASSERT(!fat.fat_markers_invalid);
}

static bool content_is(const uint8_t *expected, size_t length) {
  fat12_dirent_t entry;
  if (fat12_find(&fat, "POWER.BIN", &entry) != FAT12_OK) return false;
  if (entry.size != length) return false;
  fat12_file_t file;
  if (fat12_open(&fat, &entry, &file) != FAT12_OK) return false;
  fat12_result_t result = fat12_read(&file, read_buffer, sizeof(read_buffer));
  return result.error == FAT12_OK && result.count == length &&
      memcmp(read_buffer, expected, length) == 0;
}

TEST(test_verified_track_failure_boundaries_preserve_old_or_new) {
  fill_pattern(data_a, 1500, 16);
  fill_pattern(data_b, 1500, 17);
  for (int boundary = 0; boundary < 5; boundary++) {
    for (int apply = 0; apply < 2; apply++) {
      fault_disk_t fault;
      fault_init(&fault);
      ASSERT_EQ(fat12_init(&fat, fault_io(&fault)), FAT12_OK);
      write_file(&fat, "POWER.BIN", data_a, 1500);

      fat12_writer_t writer;
      ASSERT_EQ(fat12_open_write(&fat, "POWER.BIN", &writer), FAT12_OK);
      fat12_result_t written = fat12_write(&writer, data_b, 1500);
      ASSERT_EQ(written.error, FAT12_OK);
      fault.writes_before_failure = boundary;
      fault.apply_failed_write = apply != 0;
      fat12_err_t closed = fat12_close_write(&writer);
      if (closed == FAT12_OK) continue;
      ASSERT_EQ(closed, FAT12_ERR_WRITE);

      ASSERT_EQ(fat12_init(&fat, disk_io(&fault.disk)), FAT12_OK);
      ASSERT(content_is(data_a, 1500) || content_is(data_b, 1500));
      fat12_fsck_t report;
      ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
      ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
      ASSERT_EQ(report.lost_clusters, 0);
      ASSERT_EQ(report.broken_chains, 0);
      ASSERT_EQ(report.crosslinked, 0);
    }
  }
}

TEST(test_torn_metadata_track_is_detected_and_repaired) {
  torn_disk_t torn;
  memset(&torn, 0, sizeof(torn));
  vdisk_format_valid(&torn.disk);
  ASSERT_EQ(fat12_init(&fat, torn_io(&torn)), FAT12_OK);
  fill_pattern(data_a, 1500, 18);
  fill_pattern(data_b, 1500, 19);
  write_file(&fat, "POWER.BIN", data_a, 1500);

  fat12_writer_t writer;
  ASSERT_EQ(fat12_open_write(&fat, "POWER.BIN", &writer), FAT12_OK);
  fat12_result_t written = fat12_write(&writer, data_b, 1500);
  ASSERT_EQ(written.error, FAT12_OK);
  ASSERT_EQ(written.count, 1500);
  torn.writes_before_tear = 1;
  torn.tear_next_write = true;
  ASSERT_EQ(fat12_close_write(&writer), FAT12_ERR_WRITE);

  ASSERT_EQ(fat12_init(&fat, disk_io(&torn.disk)), FAT12_OK);
  ASSERT(fat.fat_mismatch);
  ASSERT(content_is(data_a, 1500));
  fat12_fsck_t report;
  ASSERT_EQ(fat12_fsck(&fat, &report, true), FAT12_OK);
  ASSERT(report.repaired_fat2);
  ASSERT_EQ(fat12_fsck(&fat, &report, false), FAT12_OK);
  ASSERT_EQ(report.lost_clusters, 0);
  ASSERT(!report.fat_mismatch);
  ASSERT(content_is(data_a, 1500));
}

TEST(test_fsck_dry_run_is_repeatable_and_immutable) {
  mount_clean();
  vdisk_set_fat_entry(&disk, 100, 100);
  vdisk_set_fat_entry(&disk, 200, 0x0FFFu);
  raw_dirent(&disk, 0, "LOOP    ", "BIN", FAT12_ATTR_ARCHIVE, 100, 1024);

  uint64_t digest = disk_digest(&disk);
  int sector_writes = disk.write_count;
  int track_writes = disk.track_writes;
  fat12_fsck_t first;
  fat12_fsck_t second;
  ASSERT_EQ(fat12_fsck(&fat, &first, false), FAT12_OK);
  ASSERT_EQ(first.loops, 1);
  ASSERT_EQ(first.lost_clusters, 1);
  ASSERT(!first.fat_mismatch);
  ASSERT_EQ(disk.write_count, sector_writes);
  ASSERT_EQ(disk.track_writes, track_writes);
  ASSERT_EQ(disk_digest(&disk), digest);

  ASSERT_EQ(fat12_fsck(&fat, &second, false), FAT12_OK);
  ASSERT_EQ(second.loops, first.loops);
  ASSERT_EQ(second.lost_clusters, first.lost_clusters);
  ASSERT_EQ(second.broken_chains, first.broken_chains);
  ASSERT_EQ(second.fat1_score, first.fat1_score);
  ASSERT_EQ(second.fat2_score, first.fat2_score);
  ASSERT_EQ(second.authoritative_fat, first.authoritative_fat);
  ASSERT_EQ(disk.write_count, sector_writes);
  ASSERT_EQ(disk.track_writes, track_writes);
  ASSERT_EQ(disk_digest(&disk), digest);

  repair_to_convergence(&fat);
  ASSERT_EQ(fat12_fsck(&fat, &first, false), FAT12_OK);
  ASSERT_EQ(first.loops, 0);
  ASSERT_EQ(first.lost_clusters, 0);
  ASSERT(!first.fat_mismatch);
  ASSERT(!first.repair_pending);
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

TEST(test_format_uses_persistent_workspace_and_exact_error) {
  vdisk_init(&disk);
  fat12_io_t write_only = {.write = vdisk_write, .ctx = &disk};
  fat12_io_t read_only = {.read = vdisk_read, .ctx = &disk};
  ASSERT_EQ(fat12_format(&fat, write_only, "TEST", false,
                         NULL, NULL), FAT12_ERR_INVALID);
  ASSERT_EQ(fat12_format(&fat, read_only, "TEST", false,
                         NULL, NULL), FAT12_ERR_INVALID);
  progress_calls = 0;
  progress_done = 0;
  progress_total = 0;
  ASSERT_EQ(fat12_format(&fat, disk_io(&disk), "TEST DISK", false,
                         progress, NULL), FAT12_OK);
  ASSERT_EQ(progress_calls, 2);
  ASSERT_EQ(progress_done, progress_total);
  ASSERT_EQ(fat12_init(&fat, disk_io(&disk)), FAT12_OK);

  fault_disk_t fault;
  fault_init(&fault);
  fault.writes_before_failure = 0;
  ASSERT_EQ(fat12_format(&fat, fault_io(&fault), "TEST", true,
                         NULL, NULL), FAT12_ERR_WRITE);
  ASSERT_EQ(fat12_last_io(&fat), BLOCK_ERR_VERIFY);
  ASSERT_EQ(fat12_format(&fat, fault_io(&fault), "LABEL-TOO-LONG", false,
                         NULL, NULL), FAT12_ERR_INVALID);
}

int main(void) {
  printf("=== FAT12 Tests ===\n\n");
  RUN_TEST(test_strict_geometry_and_little_endian);
  RUN_TEST(test_init_reads_every_fat_copy);
  RUN_TEST(test_canonical_name_api);
  RUN_TEST(test_open_is_fallible_and_null_safe);
  RUN_TEST(test_create_write_seek_read_delete_rename);
  RUN_TEST(test_typed_read_preserves_partial_progress);
  RUN_TEST(test_sequential_read_has_linear_fat_io);
  RUN_TEST(test_read_rejects_cycles_without_replaying_data);
  RUN_TEST(test_typed_write_preserves_partial_progress_and_retries);
  RUN_TEST(test_close_retries_each_commit_phase);
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
  RUN_TEST(test_fsck_traverses_more_than_sixteen_directories);
  RUN_TEST(test_fsck_removes_duplicate_directory_reference);
  RUN_TEST(test_fsck_removes_later_duplicate_name_and_reclaims_chain);
  RUN_TEST(test_duplicate_repair_removes_name_before_reclaim);
  RUN_TEST(test_fsck_multi_action_repair_plan_is_stable);
  RUN_TEST(test_fsck_read_failure_never_mutates);
  RUN_TEST(test_fsck_finishes_directory_reads_before_writes);
  RUN_TEST(test_fsck_namespace_plan_converges_when_full);
  RUN_TEST(test_fsck_bad_union_survives_duplicate_removal);
  RUN_TEST(test_fsck_repairs_identical_invalid_markers);
  RUN_TEST(test_marker_repair_converges_across_write_failure);
  RUN_TEST(test_verified_track_failure_boundaries_preserve_old_or_new);
  RUN_TEST(test_torn_metadata_track_is_detected_and_repaired);
  RUN_TEST(test_fsck_dry_run_is_repeatable_and_immutable);
  RUN_TEST(test_format_uses_persistent_workspace_and_exact_error);
  TEST_RESULTS();
}
