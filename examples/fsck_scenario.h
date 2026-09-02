#ifndef FSCK_SCENARIO_H
#define FSCK_SCENARIO_H

#include "fat12.h"

typedef struct {
  const char *name;
  uint32_t size;
} fsck_scenario_file_t;

typedef struct {
  uint16_t cluster;
  uint16_t value;
} fsck_scenario_patch_t;

#define FSCK_SCENARIO_FILE_COUNT 4u
#define FSCK_SCENARIO_PATCH_COUNT 4u
#define FSCK_SCENARIO_TRUNCATED_FILE 3u
#define FSCK_SCENARIO_TRUNCATED_SIZE (2u * DISK_SECTOR_SIZE)
#define FSCK_SCENARIO_MIRROR_CLUSTER 2u
#define FSCK_SCENARIO_MIRROR_VALUE 0x0FFFu

static const fsck_scenario_file_t fsck_scenario_files[FSCK_SCENARIO_FILE_COUNT] = {
    {"ALPHA.BIN", 3u * DISK_SECTOR_SIZE},
    {"BRAVO.BIN", 2u * DISK_SECTOR_SIZE},
    {"CHARLIE.BIN", DISK_SECTOR_SIZE},
    {"DELTA.BIN", 4u * DISK_SECTOR_SIZE},
};

static const fsck_scenario_patch_t fsck_scenario_patches[FSCK_SCENARIO_PATCH_COUNT] = {
    {6, 3},
    {7, 7},
    {9, 0x0FFF},
    {2000, 0x0FFF},
};

static inline bool fsck_scenario_damage_matches(const fat12_fsck_t *report) {
  return report->files == FSCK_SCENARIO_FILE_COUNT && report->directories == 0 &&
         report->lost_clusters == 3 && report->crosslinked == 1 &&
         report->loops == 1 && report->broken_chains == 2 &&
         report->size_mismatches == 3 && report->duplicate_names == 0 &&
         !report->fat_mismatch && !report->fat_markers_invalid;
}

static inline bool fsck_scenario_repair_matches(const fat12_fsck_t *report) {
  return fsck_scenario_damage_matches(report) && report->truncated_files == 1 &&
         report->removed_directories == 0 && report->removed_duplicates == 0 &&
         report->freed == 3 && report->freed_tails == 2;
}

static inline bool fsck_scenario_mirror_matches(const fat12_fsck_t *report) {
  return report->fat_mismatch && !report->fat_ambiguous &&
         report->authoritative_fat == 1 && report->fat1_score == 0 &&
         report->fat2_score == 2 && report->lost_clusters == 0 &&
         report->broken_chains == 0 && report->crosslinked == 0 &&
         report->size_mismatches == 0;
}

#endif
