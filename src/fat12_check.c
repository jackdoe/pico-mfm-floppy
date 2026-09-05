#include "internal/fat12_internal.h"
#include <string.h>

typedef enum {
  FSCK_REPORT,
  FSCK_REPAIR_FAT,
  FSCK_REPAIR_DIR,
} fsck_mode_t;

typedef enum {
  LINK_NEXT,
  LINK_END,
  LINK_BAD,
  LINK_BROKEN,
  LINK_LOOP,
  LINK_CROSS,
} fsck_link_t;

typedef struct {
  fat12_t *fat;
  fat12_fsck_t *out;
  fsck_mode_t mode;
  uint16_t next_owner;
  uint16_t freed;
  uint16_t truncated;
  uint16_t removed;
  uint16_t duplicates_removed;
  uint16_t tail_cuts;
  uint16_t fat_start;
} fsck_ctx_t;

typedef struct {
  uint16_t lba;
  uint16_t offset;
} fsck_slot_t;

typedef struct {
  bool mismatch;
  bool valid[FAT12_NUM_FATS];
} fat12_mirrors_t;

static disk_err_t fsck_get_entry(fsck_ctx_t *ctx, uint16_t cluster,
                                 uint16_t *next) {
  return fat12_resolve_entry(ctx->fat, ctx->fat_start, cluster, next);
}

static void fsck_bit_set(uint8_t *map, uint16_t cluster) {
  map[cluster / 8u] |= (uint8_t)(1u << (cluster % 8u));
}

static bool fsck_bit_test(const uint8_t *map, uint16_t cluster) {
  return (map[cluster / 8u] & (1u << (cluster % 8u))) != 0;
}

static uint16_t fsck_pending_pop(fat12_t *fat) {
  for (uint16_t byte = 0; byte < FAT12_CLUSTER_BITMAP_BYTES; byte++) {
    if (fat->fsck_pending[byte] == 0) continue;
    uint16_t bit = 0;
    while ((fat->fsck_pending[byte] & (1u << bit)) == 0) bit++;
    fat->fsck_pending[byte] &= (uint8_t)~(1u << bit);
    return (uint16_t)(byte * 8u + bit);
  }
  return 0;
}

static disk_err_t fsck_plan_fat(fsck_ctx_t *ctx, uint16_t cluster,
                                uint16_t value) {
  if (ctx->mode != FSCK_REPAIR_FAT || !fat12_cluster_valid(cluster)) {
    return DISK_OK;
  }
  disk_err_t err = fat12_set_entry(ctx->fat, cluster, value);
  if (err == DISK_OK && value == 0) ctx->freed++;
  return err;
}

static disk_err_t fsck_cut(fsck_ctx_t *ctx, uint16_t cluster) {
  disk_err_t err = fsck_plan_fat(ctx, cluster, FAT12_EOC);
  if (err == DISK_OK && ctx->mode == FSCK_REPAIR_FAT) ctx->tail_cuts++;
  return err;
}

static disk_err_t fsck_rewrite_entry(fsck_ctx_t *ctx, fsck_slot_t slot,
                                     uint16_t start_cluster, uint32_t size) {
  if (ctx->mode != FSCK_REPAIR_DIR) return DISK_OK;
  uint8_t sector[DISK_SECTOR_SIZE];
  disk_err_t err = cache_read(ctx->fat->cache, slot.lba, sector);
  if (err != DISK_OK) return err;
  fat12_dirent_t entry;
  fat12_decode_dirent(&entry, sector + slot.offset);
  entry.start_cluster = start_cluster;
  entry.size = size;
  fat12_encode_dirent(sector + slot.offset, &entry);
  err = cache_write(ctx->fat->cache, slot.lba, sector);
  if (err == DISK_OK) ctx->truncated++;
  return err;
}

static disk_err_t fsck_remove_entry(fsck_ctx_t *ctx, fsck_slot_t slot,
                                    bool directory, bool duplicate) {
  if (ctx->mode != FSCK_REPAIR_DIR) return DISK_OK;
  uint8_t sector[DISK_SECTOR_SIZE];
  disk_err_t err = cache_read(ctx->fat->cache, slot.lba, sector);
  if (err != DISK_OK) return err;
  sector[slot.offset] = FAT12_DIRENT_FREE;
  err = cache_write(ctx->fat->cache, slot.lba, sector);
  if (err != DISK_OK) return err;
  if (directory) ctx->removed++;
  if (duplicate) ctx->duplicates_removed++;
  return DISK_OK;
}

static void fsck_count_collision(fsck_ctx_t *ctx, bool loop) {
  if (loop) {
    ctx->out->loops++;
    ctx->out->broken_chains++;
  } else {
    ctx->out->crosslinked++;
  }
}

static disk_err_t fsck_follow(fsck_ctx_t *ctx, uint16_t cluster, uint16_t owner,
                              uint16_t *next, fsck_link_t *link) {
  ctx->fat->fsck_owner[cluster] = owner;
  disk_err_t err = fsck_get_entry(ctx, cluster, next);
  if (err != DISK_OK) return err;
  uint16_t value = *next;
  if (fat12_is_bad(value))
    *link = LINK_BAD;
  else if (fat12_is_eof(value))
    *link = LINK_END;
  else if (!fat12_cluster_valid(value))
    *link = LINK_BROKEN;
  else if (ctx->fat->fsck_owner[value] == owner)
    *link = LINK_LOOP;
  else if (ctx->fat->fsck_owner[value] != 0)
    *link = LINK_CROSS;
  else
    *link = LINK_NEXT;
  return DISK_OK;
}

static disk_err_t fsck_walk_file(fsck_ctx_t *ctx, const fat12_dirent_t *entry,
                                 fsck_slot_t slot) {
  uint32_t declared =
      entry->size / DISK_SECTOR_SIZE + (entry->size % DISK_SECTOR_SIZE != 0);
  bool oversized = declared > FAT12_DATA_CLUSTERS;
  uint16_t expected = oversized ? FAT12_DATA_CLUSTERS : (uint16_t)declared;
  uint16_t start = entry->start_cluster;
  if (entry->size == 0) {
    if (start == 0) return DISK_OK;
    ctx->out->size_mismatches++;
    return fsck_rewrite_entry(ctx, slot, 0, 0);
  }
  if (!fat12_cluster_valid(start)) {
    ctx->out->broken_chains++;
    ctx->out->size_mismatches++;
    return fsck_rewrite_entry(ctx, slot, 0, 0);
  }
  if (ctx->fat->fsck_owner[start] != 0) {
    ctx->out->crosslinked++;
    ctx->out->size_mismatches++;
    return fsck_rewrite_entry(ctx, slot, 0, 0);
  }

  uint16_t owner = ctx->next_owner++;
  uint16_t cluster = start;
  uint16_t previous = 0;
  uint16_t count = 0;
  for (;;) {
    count++;
    uint16_t next;
    fsck_link_t link;
    disk_err_t err = fsck_follow(ctx, cluster, owner, &next, &link);
    if (err != DISK_OK) return err;
    if (link == LINK_BAD) {
      ctx->fat->fsck_owner[cluster] = 0;
      ctx->out->broken_chains++;
      ctx->out->size_mismatches++;
      err = fsck_plan_fat(ctx, previous, FAT12_EOC);
      if (err != DISK_OK) return err;
      return fsck_rewrite_entry(ctx, slot, previous == 0 ? 0 : start,
                                (uint32_t)(count - 1u) * DISK_SECTOR_SIZE);
    }
    if (count == expected) {
      if (oversized) {
        ctx->out->broken_chains++;
        ctx->out->size_mismatches++;
        if (link != LINK_END) {
          err = fsck_cut(ctx, cluster);
          if (err != DISK_OK) return err;
        }
        return fsck_rewrite_entry(ctx, slot, start,
                                  (uint32_t)count * DISK_SECTOR_SIZE);
      }
      if (link != LINK_END) {
        ctx->out->size_mismatches++;
        if (link == LINK_LOOP || link == LINK_CROSS) {
          fsck_count_collision(ctx, link == LINK_LOOP);
        }
        return fsck_cut(ctx, cluster);
      }
      return DISK_OK;
    }
    if (link == LINK_NEXT) {
      previous = cluster;
      cluster = next;
      continue;
    }
    ctx->out->size_mismatches++;
    if (link == LINK_END || link == LINK_BROKEN)
      ctx->out->broken_chains++;
    else
      fsck_count_collision(ctx, link == LINK_LOOP);
    if (link != LINK_END) {
      err = fsck_plan_fat(ctx, cluster, FAT12_EOC);
      if (err != DISK_OK) return err;
    }
    return fsck_rewrite_entry(ctx, slot, start,
                              (uint32_t)count * DISK_SECTOR_SIZE);
  }
}

static bool fsck_namespace_entry(const fat12_dirent_t *entry) {
  return fat12_entry_valid(entry) &&
         (entry->attr & FAT12_ATTR_VOLUME_ID) == 0 && entry->name[0] != '.';
}

static bool fsck_same_name(const fat12_dirent_t *left,
                           const fat12_dirent_t *right) {
  return memcmp(left->name, right->name, FAT12_FILENAME_LEN) == 0 &&
         memcmp(left->ext, right->ext, FAT12_EXTENSION_LEN) == 0;
}

static disk_err_t fsck_duplicate(fsck_ctx_t *ctx, uint16_t directory,
                                 fsck_slot_t slot, const fat12_dirent_t *entry,
                                 bool *duplicate) {
  *duplicate = false;
  uint16_t cluster = directory;
  for (uint16_t index = 0;; index++) {
    uint16_t lba = directory == 0 ? (uint16_t)(FAT12_ROOT_START + index)
                                  : fat12_cluster_to_lba(cluster);
    uint8_t sector[DISK_SECTOR_SIZE];
    disk_err_t err = cache_read(ctx->fat->cache, lba, sector);
    if (err != DISK_OK) return err;
    for (uint16_t offset = 0; offset < DISK_SECTOR_SIZE;
         offset += FAT12_DIR_ENTRY_SIZE) {
      if (lba == slot.lba && offset == slot.offset) return DISK_OK;
      fat12_dirent_t candidate;
      fat12_decode_dirent(&candidate, sector + offset);
      if (fsck_namespace_entry(&candidate) &&
          fsck_same_name(&candidate, entry)) {
        *duplicate = true;
        return DISK_OK;
      }
    }
    if (directory != 0) {
      uint16_t next;
      err = fsck_get_entry(ctx, cluster, &next);
      if (err != DISK_OK) return err;
      cluster = next;
    }
  }
}

static disk_err_t fsck_admit_directory(fsck_ctx_t *ctx, uint16_t start,
                                       fsck_slot_t slot) {
  if (!fat12_cluster_valid(start)) {
    ctx->out->broken_chains++;
    return fsck_remove_entry(ctx, slot, true, false);
  }
  if (ctx->fat->fsck_owner[start] != 0) {
    ctx->out->crosslinked++;
    return fsck_remove_entry(ctx, slot, true, false);
  }
  uint16_t link;
  disk_err_t err = fsck_get_entry(ctx, start, &link);
  if (err != DISK_OK) return err;
  if (fat12_is_bad(link)) {
    ctx->out->broken_chains++;
    return fsck_remove_entry(ctx, slot, true, false);
  }
  ctx->fat->fsck_owner[start] = ctx->next_owner++;
  fsck_bit_set(ctx->fat->fsck_pending, start);
  return DISK_OK;
}

static disk_err_t fsck_scan_dirent(fsck_ctx_t *ctx, const uint8_t *raw,
                                   fsck_slot_t slot, uint16_t directory,
                                   bool *end) {
  fat12_dirent_t entry;
  fat12_decode_dirent(&entry, raw);
  if (fat12_entry_is_end(&entry)) {
    *end = true;
    return DISK_OK;
  }
  if (!fsck_namespace_entry(&entry)) return DISK_OK;
  bool duplicate;
  disk_err_t err = fsck_duplicate(ctx, directory, slot, &entry, &duplicate);
  if (err != DISK_OK) return err;
  bool subdirectory = (entry.attr & FAT12_ATTR_DIRECTORY) != 0;
  if (duplicate) {
    ctx->out->duplicate_names++;
    if (ctx->mode == FSCK_REPAIR_DIR) {
      return fsck_remove_entry(ctx, slot, subdirectory, true);
    }
  }
  if (subdirectory) {
    ctx->out->directories++;
    return fsck_admit_directory(ctx, entry.start_cluster, slot);
  }
  ctx->out->files++;
  return fsck_walk_file(ctx, &entry, slot);
}

static disk_err_t fsck_scan_sector(fsck_ctx_t *ctx, uint16_t lba,
                                   uint16_t directory, bool *end) {
  uint8_t sector[DISK_SECTOR_SIZE];
  disk_err_t err = cache_read(ctx->fat->cache, lba, sector);
  if (err != DISK_OK) return err;
  for (uint16_t offset = 0; offset < DISK_SECTOR_SIZE && !*end;
       offset += FAT12_DIR_ENTRY_SIZE) {
    fsck_slot_t slot = {.lba = lba, .offset = offset};
    err = fsck_scan_dirent(ctx, sector + offset, slot, directory, end);
    if (err != DISK_OK) return err;
  }
  return DISK_OK;
}

static disk_err_t fsck_walk_directory(fsck_ctx_t *ctx, uint16_t start) {
  uint16_t owner = ctx->fat->fsck_owner[start];
  uint16_t cluster = start;
  uint16_t previous = 0;
  bool end = false;
  for (;;) {
    uint16_t next;
    fsck_link_t link;
    disk_err_t err = fsck_follow(ctx, cluster, owner, &next, &link);
    if (err != DISK_OK) return err;
    if (link == LINK_BAD) {
      ctx->fat->fsck_owner[cluster] = 0;
      ctx->out->broken_chains++;
      return fsck_plan_fat(ctx, previous, FAT12_EOC);
    }
    if (!end) {
      err = fsck_scan_sector(ctx, fat12_cluster_to_lba(cluster), start, &end);
      if (err != DISK_OK) return err;
    }
    if (link == LINK_END) return DISK_OK;
    if (link == LINK_NEXT) {
      previous = cluster;
      cluster = next;
      continue;
    }
    if (link == LINK_BROKEN)
      ctx->out->broken_chains++;
    else
      fsck_count_collision(ctx, link == LINK_LOOP);
    return fsck_plan_fat(ctx, cluster, FAT12_EOC);
  }
}

static disk_err_t fsck_scan_tree(fsck_ctx_t *ctx) {
  bool end = false;
  for (uint16_t sector = 0; sector < FAT12_ROOT_DIR_SECTORS && !end; sector++) {
    disk_err_t err = fsck_scan_sector(ctx, FAT12_ROOT_START + sector, 0, &end);
    if (err != DISK_OK) return err;
  }
  for (uint16_t start; (start = fsck_pending_pop(ctx->fat)) != 0;) {
    disk_err_t err = fsck_walk_directory(ctx, start);
    if (err != DISK_OK) return err;
  }
  return DISK_OK;
}

static disk_err_t fsck_scan_lost(fsck_ctx_t *ctx) {
  fat12_scan_t scan;
  fat12_scan_init(&scan, ctx->fat, ctx->fat_start, 2);
  uint16_t cluster;
  uint16_t entry;
  disk_err_t err;
  while ((err = fat12_scan_next(&scan, FAT12_CLUSTER_LIMIT, &cluster,
                                &entry)) == DISK_OK) {
    if (entry == 0 || fat12_is_bad(entry)) continue;
    if (ctx->fat->fsck_owner[cluster] != 0) continue;
    ctx->out->lost_clusters++;
    err = fsck_plan_fat(ctx, cluster, 0);
    if (err != DISK_OK) return err;
  }
  return err == DISK_END ? DISK_OK : err;
}

static disk_err_t fsck_scan(fsck_ctx_t *ctx) {
  disk_err_t err = fsck_scan_tree(ctx);
  if (err != DISK_OK) return err;
  return fsck_scan_lost(ctx);
}

static disk_err_t fsck_compare_fats(fat12_t *fat, fat12_mirrors_t *mirrors) {
  uint8_t primary[DISK_SECTOR_SIZE];
  uint8_t copy[DISK_SECTOR_SIZE];
  memset(mirrors, 0, sizeof(*mirrors));
  for (uint16_t sector = 0; sector < FAT12_SECTORS_PER_FAT; sector++) {
    disk_err_t err =
        cache_read(fat->cache, (uint16_t)(FAT12_FAT1_START + sector), primary);
    if (err != DISK_OK) return err;
    if (sector == 0) {
      mirrors->valid[0] = primary[0] == FAT12_MEDIA_DESCRIPTOR &&
                          primary[1] == 0xFF && primary[2] == 0xFF;
    }
    for (uint8_t index = 1; index < FAT12_NUM_FATS; index++) {
      uint16_t lba =
          (uint16_t)(FAT12_FAT1_START +
                     (uint16_t)index * FAT12_SECTORS_PER_FAT + sector);
      err = cache_read(fat->cache, lba, copy);
      if (err != DISK_OK) return err;
      if (sector == 0) {
        mirrors->valid[index] = copy[0] == FAT12_MEDIA_DESCRIPTOR &&
                                copy[1] == 0xFF && copy[2] == 0xFF;
      }
      if (memcmp(primary, copy, DISK_SECTOR_SIZE) != 0) {
        mirrors->mismatch = true;
      }
    }
  }
  return DISK_OK;
}

static void fsck_reset(fsck_ctx_t *ctx, fat12_t *fat, fat12_fsck_t *out,
                       fsck_mode_t mode, uint16_t fat_start) {
  memset(fat->fsck_owner, 0, sizeof(fat->fsck_owner));
  memset(fat->fsck_pending, 0, sizeof(fat->fsck_pending));
  memset(ctx, 0, sizeof(*ctx));
  ctx->fat = fat;
  ctx->out = out;
  ctx->mode = mode;
  ctx->next_owner = 1;
  ctx->fat_start = fat_start;
}

static void fsck_capture_reachable(fat12_t *fat) {
  memset(fat->fsck_reachable, 0, sizeof(fat->fsck_reachable));
  for (uint16_t cluster = 2; cluster < FAT12_CLUSTER_LIMIT; cluster++) {
    if (fat->fsck_owner[cluster] != 0)
      fsck_bit_set(fat->fsck_reachable, cluster);
  }
}

static bool fsck_was_reachable(const fat12_t *fat, uint16_t cluster) {
  return fsck_bit_test(fat->fsck_reachable, cluster);
}

static uint32_t fsck_structural_score(const fat12_fsck_t *report) {
  uint32_t score = 0;
  score += report->crosslinked;
  score += report->loops;
  score += report->broken_chains;
  score += report->size_mismatches;
  return score;
}

static uint32_t fsck_candidate_score(const fat12_fsck_t *report,
                                     bool marker_valid) {
  return fsck_structural_score(report) + (marker_valid ? 0u : 1u);
}

static disk_err_t fsck_reachable_equal(fat12_t *fat, bool *equal) {
  *equal = true;
  for (uint16_t cluster = 2; cluster < FAT12_CLUSTER_LIMIT; cluster++) {
    bool primary = fsck_was_reachable(fat, cluster);
    bool secondary = fat->fsck_owner[cluster] != 0;
    if (primary != secondary) {
      *equal = false;
      return DISK_OK;
    }
    if (!primary) continue;
    uint16_t first;
    uint16_t second;
    disk_err_t err =
        fat12_resolve_entry(fat, FAT12_FAT1_START, cluster, &first);
    if (err != DISK_OK) return err;
    err = fat12_resolve_entry(fat, FAT12_FAT2_START, cluster, &second);
    if (err != DISK_OK) return err;
    if (first != second && !(fat12_is_eof(first) && fat12_is_eof(second))) {
      *equal = false;
      return DISK_OK;
    }
  }
  return DISK_OK;
}

static disk_err_t fsck_select_fat(fat12_t *fat, const fat12_mirrors_t *mirrors,
                                  fat12_fsck_t *out, uint16_t *fat_start) {
  fsck_ctx_t ctx;
  fat12_fsck_t primary;
  memset(&primary, 0, sizeof(primary));
  fsck_reset(&ctx, fat, &primary, FSCK_REPORT, FAT12_FAT1_START);
  disk_err_t err = fsck_scan(&ctx);
  if (err != DISK_OK) return err;
  fsck_capture_reachable(fat);
  uint32_t primary_score = fsck_candidate_score(&primary, mirrors->valid[0]);

  if (!mirrors->mismatch) {
    *out = primary;
    out->fat1_score = primary_score;
    out->fat2_score = primary_score;
    out->authoritative_fat = 1;
    *fat_start = FAT12_FAT1_START;
    return DISK_OK;
  }

  fat12_fsck_t secondary;
  memset(&secondary, 0, sizeof(secondary));
  fsck_reset(&ctx, fat, &secondary, FSCK_REPORT, FAT12_FAT2_START);
  err = fsck_scan(&ctx);
  if (err != DISK_OK) return err;
  uint32_t secondary_score =
      fsck_candidate_score(&secondary, mirrors->valid[1]);
  bool primary_clean = primary_score == 0;
  bool secondary_clean = secondary_score == 0;
  bool choose_primary = false;
  bool choose_secondary = false;
  if (primary_clean && !secondary_clean) {
    choose_primary = true;
  } else if (!primary_clean && secondary_clean) {
    choose_secondary = true;
  } else if (primary_clean && secondary_clean) {
    bool equal;
    err = fsck_reachable_equal(fat, &equal);
    if (err != DISK_OK) return err;
    choose_primary = equal;
  } else if (!mirrors->valid[0] && !mirrors->valid[1]) {
    uint32_t primary_structure = fsck_structural_score(&primary);
    uint32_t secondary_structure = fsck_structural_score(&secondary);
    if (primary_structure == 0 && secondary_structure != 0) {
      choose_primary = true;
    } else if (primary_structure != 0 && secondary_structure == 0) {
      choose_secondary = true;
    } else if (primary_structure == 0 && secondary_structure == 0) {
      bool equal;
      err = fsck_reachable_equal(fat, &equal);
      if (err != DISK_OK) return err;
      choose_primary = equal;
    }
  }

  if (!choose_primary && !choose_secondary) {
    memset(out, 0, sizeof(*out));
    out->fat_mismatch = true;
    out->fat_markers_invalid = !mirrors->valid[0] || !mirrors->valid[1];
    out->fat_ambiguous = true;
    out->fat1_score = primary_score;
    out->fat2_score = secondary_score;
    return DISK_ERR_AMBIGUOUS;
  }

  if (choose_primary) {
    *out = primary;
    *fat_start = FAT12_FAT1_START;
  } else {
    *out = secondary;
    *fat_start = FAT12_FAT2_START;
    fsck_capture_reachable(fat);
  }
  out->fat_mismatch = true;
  out->fat1_score = primary_score;
  out->fat2_score = secondary_score;
  out->authoritative_fat = choose_primary ? 1u : 2u;
  return DISK_OK;
}

static disk_err_t fsck_stage_fat_copy(fat12_t *fat, uint16_t source,
                                      uint16_t target) {
  for (uint16_t index = 0; index < FAT12_SECTORS_PER_FAT; index++) {
    uint8_t sector[DISK_SECTOR_SIZE];
    disk_err_t err = cache_read(fat->cache, source + index, sector);
    if (err != DISK_OK) return err;
    err = cache_write(fat->cache, target + index, sector);
    if (err != DISK_OK) return err;
  }
  return DISK_OK;
}

static disk_err_t fsck_stage_markers(fat12_t *fat) {
  for (uint8_t copy = 0; copy < FAT12_NUM_FATS; copy++) {
    uint16_t lba =
        (uint16_t)(FAT12_FAT1_START + (uint16_t)copy * FAT12_SECTORS_PER_FAT);
    uint8_t sector[DISK_SECTOR_SIZE];
    disk_err_t err = cache_read(fat->cache, lba, sector);
    if (err != DISK_OK) return err;
    sector[0] = FAT12_MEDIA_DESCRIPTOR;
    sector[1] = 0xFF;
    sector[2] = 0xFF;
    err = cache_write(fat->cache, lba, sector);
    if (err != DISK_OK) return err;
  }
  return DISK_OK;
}

static disk_err_t fsck_preserve_bad_union(fat12_t *fat) {
  for (uint16_t cluster = 2; cluster < FAT12_CLUSTER_LIMIT; cluster++) {
    if (fsck_was_reachable(fat, cluster)) continue;
    uint16_t first;
    uint16_t second;
    disk_err_t err =
        fat12_resolve_entry(fat, FAT12_FAT1_START, cluster, &first);
    if (err != DISK_OK) return err;
    err = fat12_resolve_entry(fat, FAT12_FAT2_START, cluster, &second);
    if (err != DISK_OK) return err;
    if (fat12_is_bad(first) || fat12_is_bad(second)) {
      err = fat12_set_entry(fat, cluster, FAT12_BAD);
      if (err != DISK_OK) return err;
    }
  }
  return DISK_OK;
}

disk_err_t fat12_fsck(fat12_t *fat, fat12_fsck_t *out, bool repair) {
  if (fat == NULL || out == NULL) return DISK_ERR_INVALID;
  if (fat12_busy(fat)) return DISK_ERR_BUSY;
  if (repair) {
    disk_err_t err = cache_writable(fat->cache);
    if (err != DISK_OK) return err;
  }
  memset(out, 0, sizeof(*out));
  fat12_mirrors_t mirrors;
  disk_err_t err = fsck_compare_fats(fat, &mirrors);
  if (err != DISK_OK) return err;
  uint16_t selected_fat;
  err = fsck_select_fat(fat, &mirrors, out, &selected_fat);
  if (err != DISK_OK) return err;
  fat->fat_start = selected_fat;
  out->fat_mismatch = mirrors.mismatch;
  out->fat_markers_invalid = false;
  for (uint8_t index = 0; index < FAT12_NUM_FATS; index++) {
    out->fat_markers_invalid |= !mirrors.valid[index];
  }
  if (!repair) return DISK_OK;

  fat->transaction = true;
  fsck_ctx_t ctx;
  fat12_fsck_t pass;
  memset(&pass, 0, sizeof(pass));
  fsck_reset(&ctx, fat, &pass, FSCK_REPAIR_DIR, selected_fat);
  err = fsck_scan_tree(&ctx);
  out->truncated_files = ctx.truncated;
  out->removed_directories = ctx.removed;
  out->removed_duplicates = ctx.duplicates_removed;
  if (err == DISK_OK) err = cache_flush(fat->cache);
  if (err != DISK_OK) {
    fat12_transaction_end(fat);
    return err;
  }

  memset(&pass, 0, sizeof(pass));
  fsck_reset(&ctx, fat, &pass, FSCK_REPAIR_FAT, selected_fat);
  err = fsck_scan_tree(&ctx);
  if (err == DISK_OK) {
    fsck_capture_reachable(fat);
    if (mirrors.mismatch) err = fsck_preserve_bad_union(fat);
  }
  if (err == DISK_OK) err = fsck_scan_lost(&ctx);
  bool repaired_fat1 = !mirrors.valid[0];
  bool repaired_fat2 = !mirrors.valid[1];
  if (err == DISK_OK && mirrors.mismatch) {
    uint16_t target =
        selected_fat == FAT12_FAT1_START ? FAT12_FAT2_START : FAT12_FAT1_START;
    err = fsck_stage_fat_copy(fat, selected_fat, target);
    repaired_fat1 |= target == FAT12_FAT1_START;
    repaired_fat2 |= target == FAT12_FAT2_START;
  }
  if (err == DISK_OK && out->fat_markers_invalid) err = fsck_stage_markers(fat);
  if (err == DISK_OK) {
    out->freed = ctx.freed;
    out->freed_tails = ctx.tail_cuts;
    err = cache_flush(fat->cache);
  }
  if (err == DISK_OK) {
    out->repaired_fat1 = repaired_fat1;
    out->repaired_fat2 = repaired_fat2;
    fat->fat_start = FAT12_FAT1_START;
  }
  fat12_transaction_end(fat);
  return err;
}

bool fat12_fsck_clean(const fat12_fsck_t *report) {
  return report != NULL && report->lost_clusters == 0 &&
         report->crosslinked == 0 && report->loops == 0 &&
         report->broken_chains == 0 && report->size_mismatches == 0 &&
         report->truncated_files == 0 && report->duplicate_names == 0 &&
         !report->fat_mismatch && !report->fat_markers_invalid &&
         !report->fat_ambiguous;
}

disk_err_t fat12_select_active_fat(fat12_t *fat) {
  fat12_mirrors_t mirrors;
  disk_err_t error = fsck_compare_fats(fat, &mirrors);
  if (error != DISK_OK || !mirrors.mismatch) return error;
  fat12_fsck_t report;
  return fsck_select_fat(fat, &mirrors, &report, &fat->fat_start);
}
