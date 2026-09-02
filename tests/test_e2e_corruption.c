#include "sim_floppy.h"
#include "scp_fixture.h"
#include "../src/f12.h"
#include "../src/fat12.h"

typedef enum {
  FILE_ANY_NONEMPTY,
  FILE_MULTICLUSTER,
  FILE_DATA_TRACK,
} file_requirement_t;

typedef struct {
  char name[FAT12_FILENAME_LEN + FAT12_EXTENSION_LEN + 2u];
  fat12_dirent_t entry;
  uint16_t index;
  uint16_t first_lba;
  uint8_t cylinder;
  uint8_t head;
} fixture_file_t;

static pio_sim_drive_t baseline;
static pio_sim_drive_t live;
static floppy_t floppy;
static f12_t filesystem;
static bool floppy_initialized;

static uint16_t load_le16(const uint8_t *data) {
  return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8u));
}

static uint32_t load_le32(const uint8_t *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
         ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static void store_le16(uint8_t *data, uint16_t value) {
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8u);
}

static bool clone_drive(const pio_sim_drive_t *source,
                        pio_sim_drive_t *destination) {
  if (!source || !destination) return false;
  pio_sim_init(destination);
  for (uint8_t cylinder = 0; cylinder < DISK_CYLINDERS; cylinder++) {
    for (uint8_t head = 0; head < DISK_HEADS; head++) {
      const pio_sim_track_t *input = &source->tracks[cylinder][head];
      pio_sim_track_t *output = &destination->tracks[cylinder][head];
      if (input->count == 0) continue;
      if (!input->deltas || input->count > PIO_SIM_MAX_FLUX) {
        pio_sim_free(destination);
        return false;
      }
      size_t bytes = (size_t)input->count * sizeof(*output->deltas);
      output->deltas = malloc(bytes);
      if (!output->deltas) {
        pio_sim_free(destination);
        return false;
      }
      memcpy(output->deltas, input->deltas, bytes);
      output->count = input->count;
    }
  }
  return true;
}

static void setup_floppy(void) {
  ASSERT_EQ(floppy_init(&floppy, sim_test_pins()), DISK_OK);
  floppy_initialized = true;
}

static void reset_to_baseline(void) {
  if (floppy_initialized) {
    ASSERT_EQ(floppy_deinit(&floppy), DISK_OK);
    floppy_initialized = false;
  }
  pio_sim_free(&live);
  ASSERT(clone_drive(&baseline, &live));
  pio_sim_install(&live, sim_test_pins(), floppy.flux_ring, &floppy.ring_cpu);
  setup_floppy();
}

static void mount_filesystem(void) {
  ASSERT_EQ(f12_init(&filesystem, floppy_device(&floppy)), DISK_OK);
  ASSERT_EQ(f12_mount(&filesystem), DISK_OK);
}

static void assert_mounted(bool expected) {
  bool mounted = !expected;
  ASSERT_EQ(f12_is_mounted(&filesystem, &mounted), DISK_OK);
  ASSERT_EQ(mounted, expected);
}

static void assert_no_defects(const fat12_fsck_t *report) {
  ASSERT_NOT_NULL(report);
  ASSERT_EQ(report->lost_clusters, 0);
  ASSERT_EQ(report->crosslinked, 0);
  ASSERT_EQ(report->loops, 0);
  ASSERT_EQ(report->broken_chains, 0);
  ASSERT_EQ(report->size_mismatches, 0);
  ASSERT_EQ(report->truncated_files, 0);
  ASSERT_EQ(report->removed_directories, 0);
  ASSERT_EQ(report->duplicate_names, 0);
  ASSERT_EQ(report->removed_duplicates, 0);
  ASSERT_EQ(report->freed_tails, 0);
  ASSERT_EQ(report->freed, 0);
  ASSERT(!report->fat_markers_invalid);
  ASSERT(!report->fat_ambiguous);
}

static void assert_clean_fsck(const fat12_fsck_t *report) {
  assert_no_defects(report);
  ASSERT_EQ(report->fat1_score, 0);
  ASSERT_EQ(report->fat2_score, 0);
  ASSERT_EQ(report->authoritative_fat, 1);
  ASSERT(!report->fat_mismatch);
  ASSERT(!report->repaired_fat1);
  ASSERT(!report->repaired_fat2);
}

static void format_name(const fat12_dirent_t *entry, char output[13]) {
  size_t length = FAT12_FILENAME_LEN;
  while (length > 0 && entry->name[length - 1u] == ' ') length--;
  memcpy(output, entry->name, length);
  size_t extension = FAT12_EXTENSION_LEN;
  while (extension > 0 && entry->ext[extension - 1u] == ' ') extension--;
  if (extension != 0) {
    output[length++] = '.';
    memcpy(output + length, entry->ext, extension);
    length += extension;
  }
  output[length] = '\0';
}

static bool regular_nonempty(const fat12_dirent_t *entry) {
  return fat12_entry_valid(entry) && entry->size != 0 &&
         (entry->attr & (FAT12_ATTR_DIRECTORY | FAT12_ATTR_VOLUME_ID)) == 0 &&
         entry->start_cluster >= 2u &&
         entry->start_cluster < FAT12_CLUSTER_LIMIT;
}

static disk_err_t find_fixture_file(file_requirement_t requirement,
                                     fixture_file_t *selected) {
  if (!selected) return DISK_ERR_INVALID;
  memset(selected, 0, sizeof(*selected));
  for (uint16_t index = 0; index < FAT12_ROOT_ENTRIES; index++) {
    fat12_dirent_t entry;
    disk_err_t error =
        fat12_read_root_entry(&filesystem.fat, index, &entry);
    if (error != DISK_OK) return error;
    if (fat12_entry_is_end(&entry)) return DISK_ERR_NOT_FOUND;
    if (!regular_nonempty(&entry)) continue;
    uint32_t first_lba_value = FAT12_DATA_START +
        ((uint32_t)entry.start_cluster - 2u) * FAT12_SECTORS_PER_CLUSTER;
    if (first_lba_value >= DISK_SECTOR_COUNT) continue;
    uint16_t first_lba = (uint16_t)first_lba_value;
    uint8_t cylinder;
    uint8_t head;
    uint8_t sector;
    if (!disk_lba_to_chs(first_lba, &cylinder, &head, &sector)) continue;
    uint16_t track;
    if (!disk_ch_to_track(cylinder, head, &track)) continue;
    uint32_t clusters = entry.size / DISK_SECTOR_SIZE +
        (entry.size % DISK_SECTOR_SIZE != 0 ? 1u : 0u);
    if (requirement == FILE_MULTICLUSTER) {
      if (clusters < 2u) continue;
      uint16_t next;
      error = fat12_get_entry(&filesystem.fat, entry.start_cluster, &next);
      if (error != DISK_OK) return error;
      if (fat12_is_eof(next) || next < 2u || next >= FAT12_CLUSTER_LIMIT) {
        continue;
      }
    }
    uint16_t first_data_track = (uint16_t)(
        (FAT12_DATA_START + DISK_SECTORS_PER_TRACK - 1u) /
        DISK_SECTORS_PER_TRACK);
    if (requirement == FILE_DATA_TRACK && track < first_data_track) continue;
    fat12_file_t reader;
    error = fat12_open(&filesystem.fat, &entry, &reader);
    if (error != DISK_OK) continue;
    selected->entry = entry;
    selected->index = index;
    selected->first_lba = first_lba;
    selected->cylinder = cylinder;
    selected->head = head;
    format_name(&entry, selected->name);
    return DISK_OK;
  }
  return DISK_ERR_NOT_FOUND;
}

static void read_file_exact(const fixture_file_t *selected, uint8_t *data) {
  ASSERT_NOT_NULL(selected);
  ASSERT_NOT_NULL(data);
  f12_file_t file;
  ASSERT_EQ(f12_open(&filesystem, selected->name, F12_OPEN_READ, &file),
            DISK_OK);
  size_t total = 0;
  size_t size = selected->entry.size;
  while (total < size) {
    disk_result_t result = f12_read(&file, data + total, size - total);
    ASSERT_EQ(result.error, DISK_OK);
    ASSERT(result.count > 0);
    ASSERT(result.count <= size - total);
    total += result.count;
  }
  uint8_t byte = 0;
  disk_result_t end = f12_read(&file, &byte, sizeof(byte));
  ASSERT_EQ(end.error, DISK_END);
  ASSERT_EQ(end.count, 0);
  ASSERT_EQ(f12_close(&file), DISK_OK);
}

static disk_err_t read_canonical_track(uint16_t lba, track_t *track,
                                           uint8_t *sector) {
  if (!track || !sector) return DISK_ERR_INVALID;
  uint8_t cylinder;
  uint8_t head;
  if (!disk_lba_to_chs(lba, &cylinder, &head, sector)) {
    return DISK_ERR_INVALID;
  }
  disk_device_t device = floppy_device(&floppy);
  uint32_t generation;
  disk_err_t status =
      device.media_generation(device.ctx, &generation);
  if (status != DISK_OK) return status;
  memset(track, 0, sizeof(*track));
  status = device.read_track(device.ctx, generation, cylinder, head, track);
  if (status != DISK_OK) return status;
  if (track->cylinder != cylinder || track->head != head ||
      track->valid != DISK_TRACK_VALID) {
    return DISK_ERR_CORRUPT;
  }
  return DISK_OK;
}

static void replace_canonical_track(const track_t *track) {
  ASSERT_NOT_NULL(track);
  ASSERT(pio_sim_replace_track(&live, track));
}

static void corrupt_fat_entry(uint16_t fat_start, uint16_t cluster) {
  size_t offset = (size_t)cluster + (size_t)cluster / 2u;
  size_t first_sector_offset = offset / DISK_SECTOR_SIZE;
  size_t second_sector_offset = (offset + 1u) / DISK_SECTOR_SIZE;
  ASSERT(first_sector_offset < FAT12_SECTORS_PER_FAT);
  ASSERT(second_sector_offset < FAT12_SECTORS_PER_FAT);
  size_t first_lba_value = (size_t)fat_start + first_sector_offset;
  size_t second_lba_value = (size_t)fat_start + second_sector_offset;
  ASSERT(first_lba_value < DISK_SECTOR_COUNT);
  ASSERT(second_lba_value < DISK_SECTOR_COUNT);
  uint16_t first_lba = (uint16_t)first_lba_value;
  uint16_t second_lba = (uint16_t)second_lba_value;
  track_t tracks[2];
  uint8_t sectors[2];
  ASSERT_EQ(read_canonical_track(first_lba, &tracks[0], &sectors[0]),
            DISK_OK);
  uint8_t second_track = 0;
  uint8_t second_cylinder;
  uint8_t second_head;
  uint8_t ignored_sector;
  ASSERT(disk_lba_to_chs(second_lba, &second_cylinder, &second_head,
                         &ignored_sector));
  if (tracks[0].cylinder != second_cylinder ||
      tracks[0].head != second_head) {
    second_track = 1;
    ASSERT_EQ(read_canonical_track(second_lba, &tracks[1], &sectors[1]),
              DISK_OK);
  } else {
    sectors[1] = ignored_sector;
  }
  size_t first_byte = offset % DISK_SECTOR_SIZE;
  size_t second_byte = (offset + 1u) % DISK_SECTOR_SIZE;
  uint8_t *low = &tracks[0].data[sectors[0]][first_byte];
  uint8_t *high = &tracks[second_track].data[sectors[1]][second_byte];
  if ((cluster & 1u) != 0) {
    *low = (uint8_t)((*low & 0x0Fu) | 0xF0u);
    *high = 0xFFu;
  } else {
    *low = 0xFFu;
    *high = (uint8_t)((*high & 0xF0u) | 0x0Fu);
  }
  replace_canonical_track(&tracks[0]);
  if (second_track != 0) replace_canonical_track(&tracks[1]);
}

static void corrupt_root_start_cluster(const fixture_file_t *selected) {
  uint32_t byte_offset = (uint32_t)selected->index * FAT12_DIR_ENTRY_SIZE;
  uint32_t root_start = FAT12_RESERVED_SECTORS +
      FAT12_NUM_FATS * FAT12_SECTORS_PER_FAT;
  uint32_t lba_value = root_start + byte_offset / DISK_SECTOR_SIZE;
  ASSERT(lba_value < DISK_SECTOR_COUNT);
  uint16_t lba = (uint16_t)lba_value;
  uint8_t sector;
  track_t track;
  ASSERT_EQ(read_canonical_track(lba, &track, &sector), DISK_OK);
  size_t offset = byte_offset % DISK_SECTOR_SIZE;
  uint8_t *raw = track.data[sector] + offset;
  ASSERT_MEM_EQ(raw, selected->entry.name, FAT12_FILENAME_LEN);
  ASSERT_MEM_EQ(raw + FAT12_FILENAME_LEN, selected->entry.ext,
                FAT12_EXTENSION_LEN);
  size_t cluster_offset = offsetof(fat12_dirent_t, start_cluster);
  size_t size_offset = offsetof(fat12_dirent_t, size);
  ASSERT_EQ(load_le16(raw + cluster_offset), selected->entry.start_cluster);
  ASSERT_EQ(load_le32(raw + size_offset), selected->entry.size);
  store_le16(raw + cluster_offset, 0);
  replace_canonical_track(&track);
}

static void lose_data_track(const fixture_file_t *selected) {
  track_t track;
  uint8_t sector;
  ASSERT_EQ(read_canonical_track(selected->first_lba, &track, &sector),
            DISK_OK);
  ASSERT_EQ(track.cylinder, selected->cylinder);
  ASSERT_EQ(track.head, selected->head);
  ASSERT(sector < DISK_SECTORS_PER_TRACK);
  replace_canonical_track(&track);
  pio_sim_track_t *flux =
      &live.tracks[selected->cylinder][selected->head];
  ASSERT_NOT_NULL(flux->deltas);
  ASSERT(flux->count > 0);
  flux->count = 0;
  if (live.head_track == selected->cylinder &&
      live.head_side == selected->head) {
    live.read_buf = NULL;
    live.read_count = 0;
    live.read_pos = 0;
  }
}

TEST(test_clean_fixture_contract) {
  reset_to_baseline();
  mount_filesystem();
  assert_mounted(true);

  fat12_fsck_t report;
  ASSERT_EQ(f12_fsck(&filesystem, &report, false), DISK_OK);
  assert_clean_fsck(&report);
  ASSERT(report.files > 0);

  f12_dir_t directory;
  ASSERT_EQ(f12_opendir(&filesystem, "/", &directory), DISK_OK);
  size_t entries = 0;
  for (;;) {
    f12_stat_t stat;
    disk_err_t error = f12_readdir(&directory, &stat);
    if (error == DISK_END) break;
    ASSERT_EQ(error, DISK_OK);
    ASSERT(stat.name[0] != '\0');
    entries++;
  }
  ASSERT(entries > 0);
  ASSERT_EQ(f12_closedir(&directory), DISK_OK);

  fixture_file_t selected;
  ASSERT_EQ(find_fixture_file(FILE_MULTICLUSTER, &selected), DISK_OK);
  ASSERT_EQ(find_fixture_file(FILE_DATA_TRACK, &selected), DISK_OK);
  ASSERT_EQ(f12_unmount(&filesystem), DISK_OK);
  assert_mounted(false);
}

static void test_fat_mirror_recovery(uint16_t damaged_fat,
                                     uint8_t authority) {
  reset_to_baseline();
  mount_filesystem();
  fixture_file_t selected;
  ASSERT_EQ(find_fixture_file(FILE_MULTICLUSTER, &selected), DISK_OK);
  uint16_t next;
  ASSERT_EQ(fat12_get_entry(&filesystem.fat, selected.entry.start_cluster,
                            &next), DISK_OK);
  ASSERT(!fat12_is_eof(next));
  uint8_t *expected = malloc(selected.entry.size);
  uint8_t *actual = malloc(selected.entry.size);
  ASSERT_NOT_NULL(expected);
  ASSERT_NOT_NULL(actual);
  read_file_exact(&selected, expected);
  ASSERT_EQ(f12_unmount(&filesystem), DISK_OK);

  corrupt_fat_entry(damaged_fat, selected.entry.start_cluster);
  ASSERT_EQ(f12_mount(&filesystem), DISK_OK);
  assert_mounted(true);

  fat12_fsck_t report;
  ASSERT_EQ(f12_fsck(&filesystem, &report, false), DISK_OK);
  assert_no_defects(&report);
  ASSERT(report.fat_mismatch);
  ASSERT_EQ(report.authoritative_fat, authority);
  if (authority == 1u) {
    ASSERT_EQ(report.fat1_score, 0);
    ASSERT_EQ(report.fat2_score, 2);
  } else {
    ASSERT_EQ(report.fat1_score, 2);
    ASSERT_EQ(report.fat2_score, 0);
  }

  read_file_exact(&selected, actual);
  ASSERT_MEM_EQ(actual, expected, selected.entry.size);

  ASSERT_EQ(f12_fsck(&filesystem, &report, true), DISK_OK);
  assert_no_defects(&report);
  ASSERT(report.fat_mismatch);
  ASSERT_EQ(report.authoritative_fat, authority);
  ASSERT_EQ(report.repaired_fat1, authority == 2u);
  ASSERT_EQ(report.repaired_fat2, authority == 1u);
  ASSERT_EQ(f12_unmount(&filesystem), DISK_OK);

  ASSERT_EQ(f12_mount(&filesystem), DISK_OK);
  ASSERT_EQ(f12_fsck(&filesystem, &report, false), DISK_OK);
  assert_clean_fsck(&report);
  read_file_exact(&selected, actual);
  ASSERT_MEM_EQ(actual, expected, selected.entry.size);
  ASSERT_EQ(f12_unmount(&filesystem), DISK_OK);
  free(actual);
  free(expected);
}

TEST(test_fat1_damage_selects_and_repairs_fat2) {
  test_fat_mirror_recovery(FAT12_FAT1_START, 2u);
}

TEST(test_fat2_damage_selects_and_repairs_fat1) {
  test_fat_mirror_recovery(FAT12_FAT2_START, 1u);
}

TEST(test_data_track_loss_has_exact_bounded_failure) {
  reset_to_baseline();
  mount_filesystem();
  fixture_file_t selected;
  ASSERT_EQ(find_fixture_file(FILE_DATA_TRACK, &selected), DISK_OK);
  ASSERT_EQ(f12_unmount(&filesystem), DISK_OK);

  lose_data_track(&selected);
  ASSERT_EQ(f12_mount(&filesystem), DISK_OK);
  f12_file_t file;
  ASSERT_EQ(f12_open(&filesystem, selected.name, F12_OPEN_READ, &file),
            DISK_OK);
  uint8_t sector[DISK_SECTOR_SIZE];
  disk_result_t result = f12_read(&file, sector, sizeof(sector));
  ASSERT_EQ(result.error, DISK_ERR_TIMEOUT);
  ASSERT_EQ(result.count, 0);
  uint32_t offset = UINT32_MAX;
  ASSERT_EQ(f12_tell(&file, &offset), DISK_OK);
  ASSERT_EQ(offset, 0);
  ASSERT_EQ(f12_close(&file), DISK_OK);
  ASSERT_EQ(f12_unmount(&filesystem), DISK_OK);
}

TEST(test_invalid_boot_sector_is_rejected) {
  reset_to_baseline();
  mount_filesystem();
  ASSERT_EQ(f12_unmount(&filesystem), DISK_OK);

  track_t track;
  uint8_t sector;
  ASSERT_EQ(read_canonical_track(0, &track, &sector), DISK_OK);
  track.data[sector][FAT12_BOOT_SIG_OFFSET] = 0;
  track.data[sector][FAT12_BOOT_SIG_OFFSET + 1u] = 0;
  replace_canonical_track(&track);

  ASSERT_EQ(f12_mount(&filesystem), DISK_ERR_INVALID);
  assert_mounted(false);
}

TEST(test_invalid_root_start_cluster_is_repaired) {
  reset_to_baseline();
  mount_filesystem();
  fixture_file_t selected;
  ASSERT_EQ(find_fixture_file(FILE_ANY_NONEMPTY, &selected), DISK_OK);
  uint32_t clusters = selected.entry.size / DISK_SECTOR_SIZE +
      (selected.entry.size % DISK_SECTOR_SIZE != 0 ? 1u : 0u);
  ASSERT(clusters > 0);
  ASSERT_EQ(f12_unmount(&filesystem), DISK_OK);

  corrupt_root_start_cluster(&selected);
  ASSERT_EQ(f12_mount(&filesystem), DISK_OK);

  fat12_fsck_t report;
  ASSERT_EQ(f12_fsck(&filesystem, &report, false), DISK_OK);
  ASSERT_EQ(report.broken_chains, 1);
  ASSERT_EQ(report.size_mismatches, 1);
  ASSERT_EQ(report.lost_clusters, clusters);
  ASSERT_EQ(report.crosslinked, 0);
  ASSERT_EQ(report.loops, 0);
  ASSERT(!report.fat_mismatch);

  f12_file_t file;
  ASSERT_EQ(f12_open(&filesystem, selected.name, F12_OPEN_READ, &file),
            DISK_ERR_CORRUPT);
  ASSERT_EQ(f12_fsck(&filesystem, &report, true), DISK_OK);
  ASSERT_EQ(report.broken_chains, 1);
  ASSERT_EQ(report.size_mismatches, 1);
  ASSERT_EQ(report.lost_clusters, clusters);
  ASSERT_EQ(report.truncated_files, 1);
  ASSERT_EQ(report.freed, clusters);
  ASSERT_EQ(f12_unmount(&filesystem), DISK_OK);

  ASSERT_EQ(f12_mount(&filesystem), DISK_OK);
  ASSERT_EQ(f12_fsck(&filesystem, &report, false), DISK_OK);
  assert_clean_fsck(&report);
  f12_stat_t stat;
  ASSERT_EQ(f12_stat(&filesystem, selected.name, &stat), DISK_OK);
  ASSERT_EQ(stat.size, 0);
  ASSERT_EQ(f12_open(&filesystem, selected.name, F12_OPEN_READ, &file),
            DISK_OK);
  uint8_t byte = 0;
  disk_result_t result = f12_read(&file, &byte, sizeof(byte));
  ASSERT_EQ(result.error, DISK_END);
  ASSERT_EQ(result.count, 0);
  ASSERT_EQ(f12_close(&file), DISK_OK);
  ASSERT_EQ(f12_unmount(&filesystem), DISK_OK);
}

int main(int argc, char **argv) {
  if (argc != 3 || strcmp(argv[1], "--fixture") != 0) {
    fprintf(stderr, "usage: %s --fixture PATH\n", argv[0]);
    return 2;
  }
  size_t fixture_size;
  uint8_t *fixture = scp_fixture_load(argv[2], &fixture_size);
  if (!fixture) {
    fprintf(stderr, "cannot read required SCP fixture: %s\n", argv[2]);
    return 1;
  }
  pio_sim_init(&baseline);
  pio_sim_init(&live);
  if (!pio_sim_load_scp(&baseline, fixture, fixture_size)) {
    fprintf(stderr, "invalid SCP fixture: %s\n", argv[2]);
    pio_sim_free(&live);
    pio_sim_free(&baseline);
    free(fixture);
    return 1;
  }

  printf("=== End-to-End Corruption Contracts ===\n\n");
  RUN_TEST(test_clean_fixture_contract);
  RUN_TEST(test_fat1_damage_selects_and_repairs_fat2);
  RUN_TEST(test_fat2_damage_selects_and_repairs_fat1);
  RUN_TEST(test_data_track_loss_has_exact_bounded_failure);
  RUN_TEST(test_invalid_boot_sector_is_rejected);
  RUN_TEST(test_invalid_root_start_cluster_is_repaired);

  if (floppy_initialized) {
    ASSERT_EQ(floppy_deinit(&floppy), DISK_OK);
    floppy_initialized = false;
  }
  pio_sim_free(&live);
  pio_sim_free(&baseline);
  free(fixture);
  TEST_RESULTS();
}
