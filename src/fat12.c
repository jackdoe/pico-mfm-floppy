#include "fat12.h"
#include <string.h>

#define FAT12_EOC 0x0FFFu
#define FAT12_BAD 0x0FF7u
#define FAT12_RESERVED_MIN 0x0FF0u
typedef enum {
  FSCK_REPORT,
  FSCK_REPAIR_FAT,
  FSCK_REPAIR_DIR,
} fsck_mode_t;

typedef struct {
  fat12_t *fat;
  fat12_fsck_t *out;
  fsck_mode_t mode;
  uint16_t directory_count;
  uint16_t directory_cursor;
  uint16_t next_owner;
  uint16_t freed;
  uint16_t truncated;
  uint16_t removed;
  uint16_t duplicates_removed;
  uint16_t tail_cuts;
  uint16_t fat_start;
  bool namespace_full;
} fsck_ctx_t;

typedef struct {
  fat12_t *fat;
  uint16_t cluster;
  uint16_t cached_lba;
  uint16_t cached_lba2;
  uint16_t fat_start;
  bool batched;
  uint8_t sector[DISK_SECTOR_SIZE];
  uint8_t sector2[DISK_SECTOR_SIZE];
} fat12_scan_t;

typedef struct {
  bool mismatch;
  bool valid[FAT12_NUM_FATS];
} fat12_mirrors_t;

static fat12_err_t fsck_compare_fats(fat12_t *fat, fat12_mirrors_t *mirrors);

static fat12_err_t fsck_select_fat(fat12_t *fat,
                                   const fat12_mirrors_t *mirrors,
                                   fat12_fsck_t *out,
                                   uint16_t *fat_start);

static uint16_t load_le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t load_le32(const uint8_t *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void store_le16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

static void store_le32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

static bool fat12_bpb_valid(const uint8_t *boot) {
  const uint8_t *p = boot + FAT12_BPB_OFFSET;
  uint32_t fat_entries =
      (FAT12_SECTORS_PER_FAT * DISK_SECTOR_SIZE * 2u) / 3u;
  return load_le16(p + 0) == DISK_SECTOR_SIZE &&
         p[2] == FAT12_MAX_CLUSTER_SECTORS &&
         load_le16(p + 3) == FAT12_RESERVED_SECTORS &&
         p[5] == FAT12_NUM_FATS &&
         load_le16(p + 6) == FAT12_ROOT_ENTRIES &&
         load_le16(p + 8) == DISK_SECTOR_COUNT &&
         p[10] == FAT12_MEDIA_DESCRIPTOR &&
         load_le16(p + 11) == FAT12_SECTORS_PER_FAT &&
         load_le16(p + 13) == DISK_SECTORS_PER_TRACK &&
         load_le16(p + 15) == DISK_HEADS &&
         load_le32(p + 17) == 0 &&
         load_le32(p + 21) == 0 &&
         FAT12_CLUSTER_LIMIT <= fat_entries;
}

static void fat12_encode_bpb(uint8_t *boot) {
  uint8_t *p = boot + FAT12_BPB_OFFSET;
  store_le16(p + 0, DISK_SECTOR_SIZE);
  p[2] = FAT12_MAX_CLUSTER_SECTORS;
  store_le16(p + 3, FAT12_RESERVED_SECTORS);
  p[5] = FAT12_NUM_FATS;
  store_le16(p + 6, FAT12_ROOT_ENTRIES);
  store_le16(p + 8, DISK_SECTOR_COUNT);
  p[10] = FAT12_MEDIA_DESCRIPTOR;
  store_le16(p + 11, FAT12_SECTORS_PER_FAT);
  store_le16(p + 13, DISK_SECTORS_PER_TRACK);
  store_le16(p + 15, DISK_HEADS);
  store_le32(p + 17, 0);
  store_le32(p + 21, 0);
}

static void fat12_decode_dirent(fat12_dirent_t *entry, const uint8_t *raw) {
  memcpy(entry->name, raw, FAT12_FILENAME_LEN);
  memcpy(entry->ext, raw + 8, FAT12_EXTENSION_LEN);
  entry->attr = raw[11];
  memcpy(entry->reserved, raw + 12, sizeof(entry->reserved));
  entry->time = load_le16(raw + 22);
  entry->date = load_le16(raw + 24);
  entry->start_cluster = load_le16(raw + 26);
  entry->size = load_le32(raw + 28);
}

static void fat12_encode_dirent(uint8_t *raw, const fat12_dirent_t *entry) {
  memcpy(raw, entry->name, FAT12_FILENAME_LEN);
  memcpy(raw + 8, entry->ext, FAT12_EXTENSION_LEN);
  raw[11] = entry->attr;
  memcpy(raw + 12, entry->reserved, sizeof(entry->reserved));
  store_le16(raw + 22, entry->time);
  store_le16(raw + 24, entry->date);
  store_le16(raw + 26, entry->start_cluster);
  store_le32(raw + 28, entry->size);
}

static fat12_err_t fat12_read_sector(fat12_t *fat, uint16_t lba,
                                     uint8_t out[static DISK_SECTOR_SIZE]) {
  if (lba >= DISK_SECTOR_COUNT) {
    fat->last_io = BLOCK_ERR_INVALID;
    return FAT12_ERR_READ;
  }
  fat->last_io = fat->io.read(fat->io.ctx, lba, out);
  return fat->last_io == BLOCK_OK ? FAT12_OK : FAT12_ERR_READ;
}

static fat12_err_t fat12_write_track(fat12_t *fat, const track_t *track) {
  if (fat->io.write == NULL) {
    fat->last_io = BLOCK_ERR_WRITE_PROTECTED;
    return FAT12_ERR_READ_ONLY;
  }
  fat->last_io = fat->io.write(fat->io.ctx, track);
  return fat->last_io == BLOCK_OK ? FAT12_OK : FAT12_ERR_WRITE;
}

block_status_t fat12_last_io(const fat12_t *fat) {
  return fat == NULL ? BLOCK_ERR_INVALID : fat->last_io;
}

bool fat12_busy(const fat12_t *fat) {
  return fat != NULL && fat->batch.active;
}

fat12_err_t fat12_init(fat12_t *fat, fat12_io_t io) {
  if (fat == NULL || io.read == NULL) return FAT12_ERR_INVALID;
  memset(fat, 0, sizeof(*fat));
  fat->io = io;
  fat->last_io = BLOCK_OK;
  fat->fat_start = FAT12_FAT1_START;

  uint8_t boot[DISK_SECTOR_SIZE];
  fat12_err_t err = fat12_read_sector(fat, 0, boot);
  if (err != FAT12_OK) return err;
  if (boot[FAT12_BOOT_SIG_OFFSET] != 0x55 ||
      boot[FAT12_BOOT_SIG_OFFSET + 1] != 0xAA) {
    return FAT12_ERR_INVALID;
  }

  if (!fat12_bpb_valid(boot)) return FAT12_ERR_INVALID;

  fat12_mirrors_t mirrors;
  err = fsck_compare_fats(fat, &mirrors);
  if (err != FAT12_OK) return err;
  if (mirrors.mismatch) {
    fat12_fsck_t selection;
    err = fsck_select_fat(fat, &mirrors, &selection, &fat->fat_start);
    if (err != FAT12_OK) return err;
  }

  return FAT12_OK;
}

static uint16_t fat12_entry_unpack(uint16_t cluster, uint8_t lo, uint8_t hi) {
  uint16_t value = (uint16_t)((uint16_t)lo |
                              (uint16_t)((uint16_t)hi << 8));
  return (cluster & 1u) ? (value >> 4) : (value & 0x0FFFu);
}

static void fat12_entry_pack(uint16_t cluster, uint16_t value,
                             uint8_t *lo, uint8_t *hi) {
  value &= 0x0FFFu;
  if (cluster & 1u) {
    *lo = (uint8_t)((*lo & 0x0Fu) | ((value & 0x0Fu) << 4));
    *hi = (uint8_t)(value >> 4);
  } else {
    *lo = (uint8_t)value;
    *hi = (uint8_t)((*hi & 0xF0u) | ((value >> 8) & 0x0Fu));
  }
}

typedef struct {
  uint16_t sector;
  uint16_t offset;
  bool split;
} fat12_entry_loc_t;

static fat12_entry_loc_t fat12_entry_locate(uint16_t cluster) {
  uint32_t fat_offset = (uint32_t)cluster + cluster / 2u;
  return (fat12_entry_loc_t){
      .sector = (uint16_t)(fat_offset / DISK_SECTOR_SIZE),
      .offset = (uint16_t)(fat_offset % DISK_SECTOR_SIZE),
      .split = (fat_offset % DISK_SECTOR_SIZE) == DISK_SECTOR_SIZE - 1u,
  };
}

bool fat12_is_eof(uint16_t cluster) {
  return cluster >= 0x0FF8u && cluster <= 0x0FFFu;
}

static bool fat12_is_bad(uint16_t cluster) {
  return cluster == FAT12_BAD;
}

static bool fat12_cluster_valid(uint16_t cluster) {
  return cluster >= 2 && cluster < FAT12_CLUSTER_LIMIT;
}

static uint16_t fat12_cluster_to_lba(uint16_t cluster) {
  return FAT12_DATA_START + cluster - 2u;
}

static fat12_err_t fat12_read_sector_batched(
    fat12_t *fat, uint16_t lba, uint8_t out[static DISK_SECTOR_SIZE]) {
  if (lba >= DISK_SECTOR_COUNT) {
    fat->last_io = BLOCK_ERR_INVALID;
    return FAT12_ERR_READ;
  }
  for (int index = (int)fat->namespace_batch.count - 1; index >= 0; index--) {
    if (fat->namespace_batch.lbas[index] == lba) {
      memcpy(out, fat->namespace_batch.data[index], DISK_SECTOR_SIZE);
      return FAT12_OK;
    }
  }
  if (fat->batch.active && lba >= FAT12_DATA_START &&
      fat->write_track.valid != 0) {
    uint8_t cylinder;
    uint8_t head;
    uint8_t sector;
    if (!disk_lba_to_chs(lba, &cylinder, &head, &sector)) {
      fat->last_io = BLOCK_ERR_INVALID;
      return FAT12_ERR_READ;
    }
    if (fat->write_track.cylinder == cylinder &&
        fat->write_track.head == head &&
        track_has(&fat->write_track, sector)) {
      memcpy(out, fat->write_track.data[sector], DISK_SECTOR_SIZE);
      return FAT12_OK;
    }
  }
  for (int index = (int)fat->batch.count - 1; index >= 0; index--) {
    if (fat->batch.lbas[index] == lba) {
      memcpy(out, fat->batch.data[index], DISK_SECTOR_SIZE);
      return FAT12_OK;
    }
  }
  return fat12_read_sector(fat, lba, out);
}

static fat12_err_t fat12_resolve_entry(fat12_t *fat, uint16_t fat_start,
                                       uint16_t cluster, bool batched,
                                       uint16_t *next) {
  if (next == NULL || !fat12_cluster_valid(cluster)) {
    if (next != NULL) *next = 0;
    return FAT12_ERR_INVALID;
  }

  fat12_entry_loc_t loc = fat12_entry_locate(cluster);
  if (loc.sector >= FAT12_SECTORS_PER_FAT ||
      (loc.split && loc.sector + 1u >= FAT12_SECTORS_PER_FAT)) {
    *next = 0;
    return FAT12_ERR_INVALID;
  }

  uint16_t lba = (uint16_t)(fat_start + loc.sector);
  uint8_t sector[DISK_SECTOR_SIZE];
  fat12_err_t err = batched
      ? fat12_read_sector_batched(fat, lba, sector)
      : fat12_read_sector(fat, lba, sector);
  if (err != FAT12_OK) return err;

  uint8_t hi;
  if (loc.split) {
    uint8_t next_sector[DISK_SECTOR_SIZE];
    err = batched
        ? fat12_read_sector_batched(fat, lba + 1u, next_sector)
        : fat12_read_sector(fat, lba + 1u, next_sector);
    if (err != FAT12_OK) return err;
    hi = next_sector[0];
  } else {
    hi = sector[loc.offset + 1u];
  }
  *next = fat12_entry_unpack(cluster, sector[loc.offset], hi);
  return FAT12_OK;
}

fat12_err_t fat12_get_entry(fat12_t *fat, uint16_t cluster, uint16_t *next) {
  if (fat == NULL) return FAT12_ERR_INVALID;
  return fat12_resolve_entry(fat, fat->fat_start, cluster, false, next);
}

static fat12_err_t fat12_get_entry_batched(fat12_t *fat, uint16_t cluster,
                                           uint16_t *next) {
  return fat12_resolve_entry(fat, fat->fat_start, cluster, true, next);
}

fat12_err_t fat12_read_root_entry(fat12_t *fat, uint16_t index,
                                  fat12_dirent_t *entry) {
  if (fat == NULL || entry == NULL) return FAT12_ERR_INVALID;
  if (index >= FAT12_ROOT_ENTRIES) return FAT12_ERR_EOF;
  uint16_t lba = FAT12_ROOT_START +
      (uint16_t)(((uint32_t)index * FAT12_DIR_ENTRY_SIZE) / DISK_SECTOR_SIZE);
  uint16_t offset = ((uint32_t)index * FAT12_DIR_ENTRY_SIZE) % DISK_SECTOR_SIZE;
  uint8_t sector[DISK_SECTOR_SIZE];
  fat12_err_t err = fat12_read_sector(fat, lba, sector);
  if (err != FAT12_OK) return err;
  fat12_decode_dirent(entry, sector + offset);
  return FAT12_OK;
}

bool fat12_entry_valid(const fat12_dirent_t *entry) {
  if (entry == NULL) return false;
  uint8_t first = (uint8_t)entry->name[0];
  return first != FAT12_DIRENT_END &&
         first != FAT12_DIRENT_FREE &&
         entry->attr != FAT12_ATTR_LFN;
}

bool fat12_entry_is_end(const fat12_dirent_t *entry) {
  return entry != NULL && (uint8_t)entry->name[0] == FAT12_DIRENT_END;
}

static char fat12_toupper(char value) {
  return value >= 'a' && value <= 'z' ? (char)(value - ('a' - 'A')) : value;
}

static bool fat12_name_char_valid(char value) {
  if ((value >= 'A' && value <= 'Z') ||
      (value >= 'a' && value <= 'z') ||
      (value >= '0' && value <= '9')) return true;
  switch (value) {
    case '$': case '%': case '\'': case '-': case '_': case '@': case '~':
    case '`': case '!': case '(': case ')': case '{': case '}': case '^':
    case '#': case '&':
      return true;
    default:
      return false;
  }
}

fat12_err_t fat12_name_parse(const char *input, fat12_name_t *out) {
  if (input == NULL || out == NULL || *input == '\0' || *input == '.') {
    return FAT12_ERR_INVALID;
  }
  memset(out->name, ' ', sizeof(out->name));
  memset(out->ext, ' ', sizeof(out->ext));

  size_t length = 0;
  while (*input != '\0' && *input != '.' && length < sizeof(out->name)) {
    if (!fat12_name_char_valid(*input)) return FAT12_ERR_INVALID;
    out->name[length++] = fat12_toupper(*input++);
  }
  if (length == 0 || (*input != '\0' && *input != '.')) {
    return FAT12_ERR_INVALID;
  }
  if (*input == '.') input++;

  length = 0;
  while (*input != '\0' && length < sizeof(out->ext)) {
    if (*input == '.' || !fat12_name_char_valid(*input)) {
      return FAT12_ERR_INVALID;
    }
    out->ext[length++] = fat12_toupper(*input++);
  }
  return *input == '\0' ? FAT12_OK : FAT12_ERR_INVALID;
}

static bool fat12_name_matches(const fat12_dirent_t *entry,
                               const fat12_name_t *name) {
  return memcmp(entry->name, name->name, FAT12_FILENAME_LEN) == 0 &&
         memcmp(entry->ext, name->ext, FAT12_EXTENSION_LEN) == 0;
}

fat12_err_t fat12_find(fat12_t *fat, const char *filename,
                       fat12_dirent_t *entry) {
  fat12_name_t name;
  fat12_err_t err = fat12_name_parse(filename, &name);
  if (err != FAT12_OK || fat == NULL || entry == NULL) return FAT12_ERR_INVALID;
  for (uint16_t index = 0; index < FAT12_ROOT_ENTRIES; index++) {
    err = fat12_read_root_entry(fat, index, entry);
    if (err != FAT12_OK) return err;
    if (fat12_entry_is_end(entry)) return FAT12_ERR_NOT_FOUND;
    if (fat12_entry_valid(entry) &&
        (entry->attr & FAT12_ATTR_VOLUME_ID) == 0 &&
        fat12_name_matches(entry, &name)) {
      return FAT12_OK;
    }
  }
  return FAT12_ERR_NOT_FOUND;
}

fat12_err_t fat12_read_cluster(fat12_t *fat, uint16_t cluster, uint8_t *buf) {
  if (fat == NULL || buf == NULL || !fat12_cluster_valid(cluster)) {
    return FAT12_ERR_INVALID;
  }
  uint16_t lba = fat12_cluster_to_lba(cluster);
  return fat12_read_sector(fat, lba, buf);
}

static fat12_err_t fat12_validate_file_entry(fat12_t *fat,
                                             const fat12_dirent_t *entry) {
  if ((entry->attr & (FAT12_ATTR_DIRECTORY | FAT12_ATTR_VOLUME_ID)) != 0) {
    return FAT12_ERR_INVALID;
  }
  uint32_t clusters = entry->size / DISK_SECTOR_SIZE +
      (entry->size % DISK_SECTOR_SIZE != 0);
  if (clusters == 0) {
    return entry->start_cluster == 0 ? FAT12_OK : FAT12_ERR_CORRUPT;
  }
  if (clusters > FAT12_DATA_CLUSTERS ||
      !fat12_cluster_valid(entry->start_cluster)) {
    return FAT12_ERR_CORRUPT;
  }
  uint16_t cluster = entry->start_cluster;
  for (uint32_t index = 0; index < clusters; index++) {
    if (!fat12_cluster_valid(cluster)) return FAT12_ERR_CORRUPT;
    uint16_t next;
    fat12_err_t err = fat12_get_entry(fat, cluster, &next);
    if (err != FAT12_OK) return err;
    if (index + 1u == clusters) {
      return fat12_is_eof(next) ? FAT12_OK : FAT12_ERR_CORRUPT;
    }
    if (!fat12_cluster_valid(next) || fat12_is_bad(next)) {
      return FAT12_ERR_CORRUPT;
    }
    cluster = next;
  }
  return FAT12_ERR_CORRUPT;
}

fat12_err_t fat12_open(fat12_t *fat, const fat12_dirent_t *entry,
                       fat12_file_t *file) {
  if (fat == NULL || entry == NULL || file == NULL) return FAT12_ERR_INVALID;
  fat12_err_t err = fat12_validate_file_entry(fat, entry);
  if (err != FAT12_OK) return err;
  memset(file, 0, sizeof(*file));
  file->fat = fat;
  file->start_cluster = entry->start_cluster;
  file->current_cluster = entry->start_cluster;
  file->file_size = entry->size;
  return FAT12_OK;
}

static fat12_err_t fat12_chain_step(fat12_t *fat, uint16_t cluster,
                                    uint16_t *next) {
  if (!fat12_cluster_valid(cluster)) return FAT12_ERR_CORRUPT;
  fat12_err_t err = fat12_get_entry(fat, cluster, next);
  if (err != FAT12_OK) return err;
  return fat12_is_eof(*next) || fat12_cluster_valid(*next)
      ? FAT12_OK
      : FAT12_ERR_CORRUPT;
}

static fat12_err_t fat12_chain_at(fat12_t *fat, uint16_t start,
                                  uint32_t index, uint16_t *out) {
  uint16_t current = start;
  uint16_t hare = start;
  bool cycle_check = fat12_cluster_valid(start);
  for (uint32_t step = 0; step < index; step++) {
    fat12_err_t err = fat12_chain_step(fat, current, &current);
    if (err != FAT12_OK) return err;
    if (cycle_check) {
      for (uint8_t hop = 0; hop < 2; hop++) {
        if (fat12_is_eof(hare)) {
          cycle_check = false;
          break;
        }
        err = fat12_chain_step(fat, hare, &hare);
        if (err != FAT12_OK) return err;
      }
      if (cycle_check && current == hare) return FAT12_ERR_CORRUPT;
    }
  }
  *out = current;
  return FAT12_OK;
}

fat12_err_t fat12_seek(fat12_file_t *file, uint32_t offset) {
  if (file == NULL || file->fat == NULL) return FAT12_ERR_INVALID;
  fat12_t *fat = file->fat;
  if (offset > file->file_size) offset = file->file_size;
  uint32_t steps = offset / DISK_SECTOR_SIZE;
  if (steps > FAT12_DATA_CLUSTERS) return FAT12_ERR_CORRUPT;
  uint16_t cluster;
  fat12_err_t err = fat12_chain_at(fat, file->start_cluster, steps, &cluster);
  if (err != FAT12_OK) return err;
  if (offset < file->file_size && !fat12_cluster_valid(cluster)) {
    return FAT12_ERR_CORRUPT;
  }
  file->current_cluster = cluster;
  file->bytes_read = offset;
  file->buffer_valid = false;
  return FAT12_OK;
}

static fat12_err_t fat12_read_current_cluster(fat12_file_t *file) {
  if (!file->buffer_valid) {
    fat12_err_t err = fat12_read_cluster(
        file->fat, file->current_cluster, file->cluster_buf);
    if (err != FAT12_OK) return err;
    file->buffer_valid = true;
  }
  return FAT12_OK;
}

fat12_result_t fat12_read(fat12_file_t *file, uint8_t *buf, size_t len) {
  fat12_result_t result = { .error = FAT12_OK, .count = 0 };
  if (file == NULL || file->fat == NULL || (buf == NULL && len != 0)) {
    result.error = FAT12_ERR_INVALID;
    return result;
  }
  fat12_t *fat = file->fat;
  while (len > 0 && file->bytes_read < file->file_size) {
    if (!fat12_cluster_valid(file->current_cluster) ||
        file->bytes_read / DISK_SECTOR_SIZE >= FAT12_DATA_CLUSTERS) {
      result.error = FAT12_ERR_CORRUPT;
      return result;
    }
    fat12_err_t err = fat12_read_current_cluster(file);
    if (err != FAT12_OK) {
      result.error = err;
      return result;
    }
    size_t offset = file->bytes_read % DISK_SECTOR_SIZE;
    size_t count = DISK_SECTOR_SIZE - offset;
    size_t remaining = file->file_size - file->bytes_read;
    if (count > remaining) count = remaining;
    if (count > len) count = len;
    bool advance = file->bytes_read + count < file->file_size &&
        offset + count == DISK_SECTOR_SIZE;
    uint16_t next = 0;
    if (advance) {
      err = fat12_chain_step(fat, file->current_cluster, &next);
      if (err != FAT12_OK) {
        result.error = err;
        return result;
      }
    }
    memcpy(buf, file->cluster_buf + offset, count);
    buf += count;
    len -= count;
    result.count += count;
    file->bytes_read += (uint32_t)count;

    if (advance) {
      file->current_cluster = next;
      file->buffer_valid = false;
    }
  }
  return result;
}

static bool fat12_batch_begin(fat12_t *fat) {
  if (fat->batch.active) return false;
  fat->batch.count = 0;
  fat->batch.active = true;
  fat->namespace_batch.count = 0;
  fat->namespace_batch.active = true;
  memset(&fat->write_track, 0, sizeof(fat->write_track));
  return true;
}

static void fat12_batch_abort(fat12_t *fat) {
  if (fat == NULL) return;
  fat->batch.count = 0;
  fat->batch.active = false;
  fat->namespace_batch.count = 0;
  fat->namespace_batch.active = false;
  fat->write_track.valid = 0;
}

static fat12_err_t fat12_write_batch_add(fat12_write_batch_t *batch,
                                         uint16_t lba, const uint8_t *data) {
  for (uint8_t index = 0; index < batch->count; index++) {
    if (batch->lbas[index] == lba) {
      memcpy(batch->data[index], data, DISK_SECTOR_SIZE);
      return FAT12_OK;
    }
  }
  if (batch->count >= FAT12_WRITE_BATCH_MAX) return FAT12_ERR_FULL;
  batch->lbas[batch->count] = lba;
  memcpy(batch->data[batch->count], data, DISK_SECTOR_SIZE);
  batch->count++;
  return FAT12_OK;
}

static void fat12_batch_sort(fat12_write_batch_t *batch) {
  for (uint8_t index = 1; index < batch->count; index++) {
    uint16_t lba = batch->lbas[index];
    uint8_t data[DISK_SECTOR_SIZE];
    memcpy(data, batch->data[index], sizeof(data));
    int at = index - 1;
    while (at >= 0 && batch->lbas[at] > lba) {
      batch->lbas[at + 1] = batch->lbas[at];
      memcpy(batch->data[at + 1], batch->data[at], DISK_SECTOR_SIZE);
      at--;
    }
    batch->lbas[at + 1] = lba;
    memcpy(batch->data[at + 1], data, sizeof(data));
  }
}

static fat12_err_t fat12_write_data_flush(fat12_t *fat) {
  if (fat->write_track.valid == 0) return FAT12_OK;
  fat12_err_t err = fat12_write_track(fat, &fat->write_track);
  if (err == FAT12_OK) fat->write_track.valid = 0;
  return err;
}

static fat12_err_t fat12_write_metadata_flush(fat12_t *fat,
                                              fat12_write_batch_t *batch) {
  fat12_batch_sort(batch);
  while (batch->count != 0) {
    uint8_t cylinder;
    uint8_t head;
    uint8_t ignored;
    if (!disk_lba_to_chs(batch->lbas[0], &cylinder, &head, &ignored)) {
      return FAT12_ERR_INVALID;
    }
    track_t *track = &fat->write_track;
    memset(track, 0, sizeof(*track));
    track->cylinder = cylinder;
    track->head = head;

    for (uint8_t index = 0; index < batch->count; index++) {
      uint8_t entry_cylinder;
      uint8_t entry_head;
      uint8_t sector;
      if (!disk_lba_to_chs(batch->lbas[index], &entry_cylinder,
                           &entry_head, &sector)) {
        return FAT12_ERR_INVALID;
      }
      if (entry_cylinder == cylinder && entry_head == head) {
        memcpy(track->data[sector], batch->data[index], DISK_SECTOR_SIZE);
        if (!track_mark(track, sector)) return FAT12_ERR_INVALID;
      }
    }

    fat12_err_t err = fat12_write_track(fat, track);
    track->valid = 0;
    if (err != FAT12_OK) return err;

    uint8_t retained = 0;
    for (uint8_t index = 0; index < batch->count; index++) {
      uint8_t entry_cylinder;
      uint8_t entry_head;
      uint8_t sector;
      if (!disk_lba_to_chs(batch->lbas[index], &entry_cylinder,
                           &entry_head, &sector)) {
        return FAT12_ERR_INVALID;
      }
      bool written = entry_cylinder == cylinder && entry_head == head;
      if (!written) {
        if (retained != index) {
          batch->lbas[retained] = batch->lbas[index];
          memcpy(batch->data[retained], batch->data[index], DISK_SECTOR_SIZE);
        }
        retained++;
      }
    }
    batch->count = retained;
  }
  return FAT12_OK;
}

static fat12_err_t fat12_write_batch_flush(fat12_t *fat) {
  fat12_err_t err = fat12_write_data_flush(fat);
  return err == FAT12_OK ? fat12_write_metadata_flush(fat, &fat->batch) : err;
}

static fat12_err_t fat12_write_data_sector(fat12_t *fat, uint16_t lba,
                                           const uint8_t *data) {
  uint8_t cylinder;
  uint8_t head;
  uint8_t sector;
  if (!disk_lba_to_chs(lba, &cylinder, &head, &sector)) {
    return FAT12_ERR_INVALID;
  }
  track_t *track = &fat->write_track;
  if (track->valid != 0 &&
      (track->cylinder != cylinder || track->head != head)) {
    fat12_err_t err = fat12_write_data_flush(fat);
    if (err != FAT12_OK) return err;
  }
  if (track->valid == 0) {
    track->cylinder = cylinder;
    track->head = head;
  }
  memcpy(track->data[sector], data, DISK_SECTOR_SIZE);
  if (!track_mark(track, sector)) return FAT12_ERR_INVALID;
  return track->valid == DISK_TRACK_VALID
      ? fat12_write_data_flush(fat) : FAT12_OK;
}

static fat12_err_t fat12_write_sector_batched(fat12_t *fat, uint16_t lba,
                                              const uint8_t *data) {
  if (!fat->batch.active || lba >= DISK_SECTOR_COUNT) return FAT12_ERR_INVALID;
  if (lba >= FAT12_DATA_START) {
    return fat12_write_data_sector(fat, lba, data);
  }
  return fat12_write_batch_add(&fat->batch, lba, data);
}

static fat12_err_t fat12_set_entry(fat12_t *fat, uint16_t cluster,
                                   uint16_t value) {
  if (!fat12_cluster_valid(cluster) || value > 0x0FFFu) {
    return FAT12_ERR_INVALID;
  }
  fat12_entry_loc_t loc = fat12_entry_locate(cluster);
  uint8_t sector[DISK_SECTOR_SIZE];
  uint8_t sector2[DISK_SECTOR_SIZE];
  fat12_err_t err = fat12_read_sector_batched(
      fat, (uint16_t)(FAT12_RESERVED_SECTORS + loc.sector), sector);
  if (err != FAT12_OK) return err;
  if (loc.split) {
    err = fat12_read_sector_batched(
        fat, (uint16_t)(FAT12_RESERVED_SECTORS + loc.sector + 1u),
        sector2);
    if (err != FAT12_OK) return err;
  }
  uint8_t *hi = loc.split ? &sector2[0] : &sector[loc.offset + 1u];
  fat12_entry_pack(cluster, value, &sector[loc.offset], hi);

  for (uint8_t copy = 0; copy < FAT12_NUM_FATS; copy++) {
    uint16_t lba = (uint16_t)(FAT12_RESERVED_SECTORS +
        (uint16_t)copy * FAT12_SECTORS_PER_FAT + loc.sector);
    err = fat12_write_sector_batched(fat, lba, sector);
    if (err != FAT12_OK) return err;
    if (loc.split) {
      err = fat12_write_sector_batched(fat, (uint16_t)(lba + 1u), sector2);
      if (err != FAT12_OK) return err;
    }
  }
  return FAT12_OK;
}

static void fat12_scan_init(fat12_scan_t *scan, fat12_t *fat,
                            uint16_t fat_start, bool batched,
                            uint16_t start) {
  memset(scan, 0, sizeof(*scan));
  scan->fat = fat;
  scan->cluster = start;
  scan->fat_start = fat_start;
  scan->batched = batched;
  scan->cached_lba = UINT16_MAX;
  scan->cached_lba2 = UINT16_MAX;
}

static fat12_err_t fat12_scan_next(fat12_scan_t *scan, uint16_t end,
                                   uint16_t *cluster, uint16_t *entry) {
  if (scan->cluster >= end) return FAT12_ERR_EOF;
  fat12_entry_loc_t loc = fat12_entry_locate(scan->cluster);
  uint16_t lba = (uint16_t)(scan->fat_start + loc.sector);
  uint16_t offset = loc.offset;
  if (lba != scan->cached_lba) {
    fat12_err_t err = scan->batched
        ? fat12_read_sector_batched(scan->fat, lba, scan->sector)
        : fat12_read_sector(scan->fat, lba, scan->sector);
    if (err != FAT12_OK) return err;
    scan->cached_lba = lba;
    scan->cached_lba2 = UINT16_MAX;
  }
  uint8_t hi;
  if (loc.split) {
    if (scan->cached_lba2 != lba + 1u) {
      fat12_err_t err = scan->batched
          ? fat12_read_sector_batched(scan->fat, lba + 1u, scan->sector2)
          : fat12_read_sector(scan->fat, lba + 1u, scan->sector2);
      if (err != FAT12_OK) return err;
      scan->cached_lba2 = lba + 1u;
    }
    hi = scan->sector2[0];
  } else {
    hi = scan->sector[offset + 1u];
  }
  *cluster = scan->cluster;
  *entry = fat12_entry_unpack(scan->cluster, scan->sector[offset], hi);
  scan->cluster++;
  return FAT12_OK;
}

static fat12_err_t fat12_find_free_range(fat12_t *fat, uint16_t start,
                                         uint16_t end, uint16_t *out) {
  fat12_scan_t scan;
  fat12_scan_init(&scan, fat, fat->fat_start, true, start);
  uint16_t cluster;
  uint16_t entry;
  fat12_err_t err;
  while ((err = fat12_scan_next(&scan, end, &cluster, &entry)) == FAT12_OK) {
    if (entry == 0) {
      *out = cluster;
      return FAT12_OK;
    }
  }
  return err == FAT12_ERR_EOF ? FAT12_ERR_FULL : err;
}

static fat12_err_t fat12_find_free_cluster(fat12_t *fat, uint16_t start,
                                           uint16_t *out) {
  if (start < 2 || start >= FAT12_CLUSTER_LIMIT) start = 2;
  fat12_err_t err = fat12_find_free_range(
      fat, start, FAT12_CLUSTER_LIMIT, out);
  if (err != FAT12_ERR_FULL || start == 2) return err;
  return fat12_find_free_range(fat, 2, start, out);
}

fat12_err_t fat12_free_count(fat12_t *fat, uint16_t *count) {
  if (fat == NULL || count == NULL) return FAT12_ERR_INVALID;
  fat12_scan_t scan;
  fat12_scan_init(&scan, fat, fat->fat_start, true, 2);
  uint16_t cluster;
  uint16_t entry;
  uint16_t total = 0;
  fat12_err_t err;
  while ((err = fat12_scan_next(
              &scan, FAT12_CLUSTER_LIMIT, &cluster, &entry)) == FAT12_OK) {
    if (entry == 0) total++;
  }
  if (err != FAT12_ERR_EOF) return err;
  *count = total;
  return FAT12_OK;
}

static fat12_err_t fat12_write_cluster(fat12_t *fat, uint16_t cluster,
                                       const uint8_t *buf) {
  if (!fat12_cluster_valid(cluster)) return FAT12_ERR_INVALID;
  uint16_t lba = fat12_cluster_to_lba(cluster);
  return fat12_write_sector_batched(fat, lba, buf);
}

static fat12_err_t fat12_write_root_entry(fat12_t *fat, uint16_t index,
                                          const fat12_dirent_t *entry) {
  if (index >= FAT12_ROOT_ENTRIES) return FAT12_ERR_EOF;
  uint16_t lba = FAT12_ROOT_START +
      (uint16_t)(((uint32_t)index * FAT12_DIR_ENTRY_SIZE) / DISK_SECTOR_SIZE);
  uint16_t offset = ((uint32_t)index * FAT12_DIR_ENTRY_SIZE) % DISK_SECTOR_SIZE;
  uint8_t sector[DISK_SECTOR_SIZE];
  fat12_err_t err = fat12_read_sector_batched(fat, lba, sector);
  if (err != FAT12_OK) return err;
  fat12_encode_dirent(sector + offset, entry);
  return fat12_write_sector_batched(fat, lba, sector);
}

static void fat12_init_dirent(fat12_dirent_t *entry,
                              const fat12_name_t *name) {
  memset(entry, 0, sizeof(*entry));
  memcpy(entry->name, name->name, sizeof(entry->name));
  memcpy(entry->ext, name->ext, sizeof(entry->ext));
  entry->attr = FAT12_ATTR_ARCHIVE;
}

static fat12_err_t fat12_validate_chain(fat12_t *fat, uint16_t start) {
  if (!fat12_cluster_valid(start)) return FAT12_ERR_CORRUPT;
  uint16_t cluster = start;
  for (uint16_t count = 0; count < FAT12_DATA_CLUSTERS; count++) {
    uint16_t next;
    fat12_err_t err = fat12_get_entry(fat, cluster, &next);
    if (err != FAT12_OK) return err;
    if (fat12_is_eof(next)) return FAT12_OK;
    if (!fat12_cluster_valid(next) || fat12_is_bad(next)) {
      return FAT12_ERR_CORRUPT;
    }
    cluster = next;
  }
  return FAT12_ERR_CORRUPT;
}

static fat12_err_t fat12_require_exclusive_chains(fat12_t *fat) {
  fat12_fsck_t report;
  fat12_err_t err = fat12_fsck(fat, &report, false);
  if (err != FAT12_OK) return err;
  return report.crosslinked == 0 && report.loops == 0 &&
      report.broken_chains == 0 && report.size_mismatches == 0 &&
      report.duplicate_names == 0 &&
      !report.fat_mismatch && !report.fat_markers_invalid &&
      !report.repair_pending && !report.incomplete
      ? FAT12_OK
      : FAT12_ERR_CORRUPT;
}

fat12_err_t fat12_open_write(fat12_t *fat, const char *filename,
                             fat12_writer_t *writer) {
  if (fat == NULL || writer == NULL) return FAT12_ERR_INVALID;
  memset(writer, 0, sizeof(*writer));
  if (fat->io.write == NULL) return FAT12_ERR_READ_ONLY;
  if (fat12_busy(fat)) return FAT12_ERR_BUSY;

  fat12_name_t name;
  fat12_err_t err = fat12_name_parse(filename, &name);
  if (err != FAT12_OK) return err;
  uint16_t free_index = UINT16_MAX;
  fat12_dirent_t entry;
  bool found = false;

  for (uint16_t index = 0; index < FAT12_ROOT_ENTRIES; index++) {
    err = fat12_read_root_entry(fat, index, &entry);
    if (err != FAT12_OK) return err;
    uint8_t first = (uint8_t)entry.name[0];
    if (first == FAT12_DIRENT_END) {
      if (free_index == UINT16_MAX) {
        free_index = index;
        writer->consumed_end = true;
      }
      break;
    }
    if (first == FAT12_DIRENT_FREE) {
      if (free_index == UINT16_MAX) free_index = index;
      continue;
    }
    if (!fat12_entry_valid(&entry) ||
        (entry.attr & FAT12_ATTR_VOLUME_ID) != 0) continue;
    if (fat12_name_matches(&entry, &name)) {
      if ((entry.attr & FAT12_ATTR_DIRECTORY) != 0) return FAT12_ERR_EXISTS;
      if ((entry.attr & FAT12_ATTR_READ_ONLY) != 0) return FAT12_ERR_READ_ONLY;
      if (entry.start_cluster == 0) {
        if (entry.size != 0) return FAT12_ERR_CORRUPT;
      } else {
        err = fat12_validate_chain(fat, entry.start_cluster);
        if (err != FAT12_OK) return err;
      }
      writer->dirent_index = index;
      writer->dirent = entry;
      writer->replacing_existing = true;
      found = true;
      break;
    }
  }

  if (!found) {
    if (free_index == UINT16_MAX) return FAT12_ERR_FULL;
    writer->dirent_index = free_index;
    fat12_init_dirent(&writer->dirent, &name);
  }
  err = fat12_require_exclusive_chains(fat);
  if (err != FAT12_OK) return err;
  if (!fat12_batch_begin(fat)) return FAT12_ERR_BUSY;
  writer->fat = fat;
  writer->phase = FAT12_WRITER_DATA;
  return FAT12_OK;
}

static fat12_err_t fat12_writer_alloc_cluster(fat12_writer_t *writer) {
  fat12_t *fat = writer->fat;
  fat12_err_t err;
  if (writer->pending_cluster == 0) {
    uint16_t start = writer->prev_cluster + 1u;
    if (start < 2 || start >= FAT12_CLUSTER_LIMIT) start = 2;
    err = fat12_find_free_cluster(
        fat, start, &writer->pending_cluster);
    if (err != FAT12_OK) return err;
  }
  uint16_t cluster = writer->pending_cluster;
  err = fat12_set_entry(fat, cluster, FAT12_EOC);
  if (err != FAT12_OK) return err;
  if (writer->prev_cluster != 0) {
    err = fat12_set_entry(fat, writer->prev_cluster, cluster);
    if (err != FAT12_OK) return err;
  }
  if (writer->first_cluster == 0) writer->first_cluster = cluster;
  writer->current_cluster = cluster;
  writer->pending_cluster = 0;
  memset(writer->cluster_buf, 0, sizeof(writer->cluster_buf));
  return FAT12_OK;
}

static bool fat12_writer_error_terminal(fat12_err_t err) {
  return err != FAT12_ERR_READ && err != FAT12_ERR_WRITE;
}

static uint16_t fat12_writer_offset(const fat12_writer_t *writer) {
  uint16_t offset = writer->bytes_written % DISK_SECTOR_SIZE;
  return writer->current_cluster != 0 && writer->bytes_written != 0 &&
      offset == 0 ? DISK_SECTOR_SIZE : offset;
}

static fat12_err_t fat12_writer_flush_cluster(fat12_writer_t *writer) {
  if (writer->current_cluster == 0) return FAT12_OK;
  return fat12_write_cluster(
      writer->fat, writer->current_cluster, writer->cluster_buf);
}

static fat12_err_t fat12_writer_prepare_next_cluster(fat12_writer_t *writer) {
  fat12_err_t err = fat12_writer_flush_cluster(writer);
  if (err != FAT12_OK) return err;
  if (writer->current_cluster != 0) {
    writer->prev_cluster = writer->current_cluster;
    writer->current_cluster = 0;
  }
  return fat12_writer_alloc_cluster(writer);
}

fat12_result_t fat12_write(fat12_writer_t *writer, const uint8_t *buf,
                           size_t len) {
  fat12_result_t result = { .error = FAT12_OK, .count = 0 };
  if (writer == NULL || writer->fat == NULL || (buf == NULL && len != 0)) {
    result.error = FAT12_ERR_INVALID;
    return result;
  }
  if (writer->phase != FAT12_WRITER_DATA) {
    result.error = FAT12_ERR_BUSY;
    return result;
  }
  if (writer->error != FAT12_OK) {
    result.error = writer->error;
    return result;
  }
  while (len > 0) {
    uint16_t offset = fat12_writer_offset(writer);
    if (writer->current_cluster == 0 || offset == DISK_SECTOR_SIZE) {
      fat12_err_t err = fat12_writer_prepare_next_cluster(writer);
      if (err != FAT12_OK) {
        result.error = err;
        if (fat12_writer_error_terminal(err)) writer->error = err;
        return result;
      }
      offset = 0;
    }
    size_t count = DISK_SECTOR_SIZE - offset;
    if (count > len) count = len;
    memcpy(writer->cluster_buf + offset, buf, count);
    writer->bytes_written += (uint32_t)count;
    buf += count;
    len -= count;
    result.count += count;
  }
  return result;
}

static fat12_err_t fat12_stage_free_chain(fat12_t *fat, uint16_t start) {
  uint16_t cluster = start;
  for (uint16_t count = 0; count < FAT12_DATA_CLUSTERS; count++) {
    if (!fat12_cluster_valid(cluster)) return FAT12_ERR_CORRUPT;
    uint16_t next;
    fat12_err_t err = fat12_get_entry(fat, cluster, &next);
    if (err != FAT12_OK) return err;
    err = fat12_set_entry(fat, cluster, 0);
    if (err != FAT12_OK) return err;
    if (fat12_is_eof(next)) return FAT12_OK;
    if (!fat12_cluster_valid(next) || fat12_is_bad(next)) {
      return FAT12_ERR_CORRUPT;
    }
    cluster = next;
  }
  return FAT12_ERR_CORRUPT;
}

fat12_err_t fat12_abort_write(fat12_writer_t *writer) {
  if (writer == NULL || writer->fat == NULL) return FAT12_ERR_INVALID;
  if (writer->phase == FAT12_WRITER_DONE) return FAT12_OK;
  if (writer->phase != FAT12_WRITER_DATA) return FAT12_ERR_BUSY;
  fat12_batch_abort(writer->fat);
  writer->phase = FAT12_WRITER_DONE;
  return FAT12_OK;
}

void fat12_forget_write(fat12_writer_t *writer) {
  if (writer == NULL) return;
  if (writer->fat != NULL) fat12_batch_abort(writer->fat);
  writer->fat = NULL;
  writer->phase = FAT12_WRITER_DONE;
}

fat12_err_t fat12_close_write(fat12_writer_t *writer) {
  if (writer == NULL || writer->fat == NULL) return FAT12_ERR_INVALID;
  fat12_t *fat = writer->fat;
  if (writer->phase == FAT12_WRITER_DONE) return FAT12_OK;
  if (writer->error != FAT12_OK) return writer->error;

  while (true) {
    fat12_err_t err;
    switch (writer->phase) {
      case FAT12_WRITER_DATA:
        err = fat12_writer_flush_cluster(writer);
        if (err != FAT12_OK) return err;
        writer->phase = FAT12_WRITER_FLUSH_NEW;
        break;

      case FAT12_WRITER_FLUSH_NEW:
        err = fat12_write_batch_flush(fat);
        if (err != FAT12_OK) return err;
        writer->phase = FAT12_WRITER_STAGE_DIRENT;
        break;

      case FAT12_WRITER_STAGE_DIRENT:
        {
          fat12_dirent_t published = writer->dirent;
          published.start_cluster = writer->first_cluster;
          published.size = writer->bytes_written;
          err = fat12_write_root_entry(
              fat, writer->dirent_index, &published);
          if (err == FAT12_OK && writer->consumed_end &&
              writer->dirent_index + 1u < FAT12_ROOT_ENTRIES) {
            fat12_dirent_t end;
            memset(&end, 0, sizeof(end));
            err = fat12_write_root_entry(
                fat, writer->dirent_index + 1u, &end);
          }
        }
        if (err != FAT12_OK) return err;
        writer->phase = FAT12_WRITER_FLUSH_DIRENT;
        break;

      case FAT12_WRITER_FLUSH_DIRENT:
        err = fat12_write_batch_flush(fat);
        if (err != FAT12_OK) return err;
        writer->phase = FAT12_WRITER_PREPARE_RECLAIM;
        break;

      case FAT12_WRITER_PREPARE_RECLAIM:
        if (!writer->replacing_existing || writer->dirent.start_cluster < 2) {
          writer->phase = FAT12_WRITER_DONE;
          fat12_batch_abort(fat);
          return FAT12_OK;
        }
        err = fat12_stage_free_chain(fat, writer->dirent.start_cluster);
        if (err != FAT12_OK) return err;
        writer->phase = FAT12_WRITER_FLUSH_RECLAIM;
        break;

      case FAT12_WRITER_FLUSH_RECLAIM:
        err = fat12_write_batch_flush(fat);
        if (err != FAT12_OK) return err;
        writer->phase = FAT12_WRITER_DONE;
        fat12_batch_abort(fat);
        return FAT12_OK;

      case FAT12_WRITER_DONE:
        return FAT12_OK;
    }
  }
}

fat12_err_t fat12_delete(fat12_t *fat, const char *filename) {
  if (fat == NULL) return FAT12_ERR_INVALID;
  if (fat->io.write == NULL) return FAT12_ERR_READ_ONLY;
  if (fat12_busy(fat)) return FAT12_ERR_BUSY;
  fat12_name_t name;
  fat12_err_t err = fat12_name_parse(filename, &name);
  if (err != FAT12_OK) return err;

  fat12_dirent_t entry;
  uint16_t index;
  for (index = 0; index < FAT12_ROOT_ENTRIES; index++) {
    err = fat12_read_root_entry(fat, index, &entry);
    if (err != FAT12_OK) return err;
    if (fat12_entry_is_end(&entry)) return FAT12_ERR_NOT_FOUND;
    if (!fat12_entry_valid(&entry) ||
        (entry.attr & (FAT12_ATTR_VOLUME_ID | FAT12_ATTR_DIRECTORY)) != 0) {
      continue;
    }
    if (fat12_name_matches(&entry, &name)) break;
  }
  if (index == FAT12_ROOT_ENTRIES) return FAT12_ERR_NOT_FOUND;
  if ((entry.attr & FAT12_ATTR_READ_ONLY) != 0) return FAT12_ERR_READ_ONLY;
  err = fat12_require_exclusive_chains(fat);
  if (err != FAT12_OK) return err;

  uint16_t start = entry.start_cluster;
  if (start == 0) {
    if (entry.size != 0) return FAT12_ERR_CORRUPT;
  } else {
    err = fat12_validate_chain(fat, start);
    if (err != FAT12_OK) return err;
  }
  if (!fat12_batch_begin(fat)) return FAT12_ERR_BUSY;
  entry.name[0] = (char)FAT12_DIRENT_FREE;
  err = fat12_write_root_entry(fat, index, &entry);
  if (err == FAT12_OK) err = fat12_write_batch_flush(fat);
  if (err == FAT12_OK && start >= 2) {
    err = fat12_stage_free_chain(fat, start);
    if (err == FAT12_OK) err = fat12_write_batch_flush(fat);
  }
  fat12_batch_abort(fat);
  return err;
}

fat12_err_t fat12_rename(fat12_t *fat, const char *from, const char *to) {
  if (fat == NULL) return FAT12_ERR_INVALID;
  if (fat->io.write == NULL) return FAT12_ERR_READ_ONLY;
  if (fat12_busy(fat)) return FAT12_ERR_BUSY;
  fat12_name_t source;
  fat12_name_t target;
  fat12_err_t err = fat12_name_parse(from, &source);
  if (err != FAT12_OK) return err;
  err = fat12_name_parse(to, &target);
  if (err != FAT12_OK) return err;

  fat12_dirent_t entry;
  err = fat12_find(fat, to, &entry);
  if (err == FAT12_OK) return FAT12_ERR_EXISTS;
  if (err != FAT12_ERR_NOT_FOUND) return err;
  for (uint16_t index = 0; index < FAT12_ROOT_ENTRIES; index++) {
    err = fat12_read_root_entry(fat, index, &entry);
    if (err != FAT12_OK) return err;
    if (fat12_entry_is_end(&entry)) return FAT12_ERR_NOT_FOUND;
    if (!fat12_entry_valid(&entry) ||
        (entry.attr & (FAT12_ATTR_VOLUME_ID | FAT12_ATTR_DIRECTORY)) != 0) {
      continue;
    }
    if (!fat12_name_matches(&entry, &source)) continue;
    if ((entry.attr & FAT12_ATTR_READ_ONLY) != 0) return FAT12_ERR_READ_ONLY;
    memcpy(entry.name, target.name, sizeof(entry.name));
    memcpy(entry.ext, target.ext, sizeof(entry.ext));
    if (!fat12_batch_begin(fat)) return FAT12_ERR_BUSY;
    err = fat12_write_root_entry(fat, index, &entry);
    if (err == FAT12_OK) err = fat12_write_batch_flush(fat);
    fat12_batch_abort(fat);
    return err;
  }
  return FAT12_ERR_NOT_FOUND;
}

static bool fsck_owner_valid(uint16_t cluster) {
  return fat12_cluster_valid(cluster);
}

static fat12_err_t fsck_get_entry(fsck_ctx_t *ctx, uint16_t cluster,
                                  uint16_t *next) {
  return fat12_resolve_entry(ctx->fat, ctx->fat_start, cluster,
                             ctx->fat->batch.active, next);
}

static uint16_t fsck_location(uint16_t lba, uint16_t offset) {
  return (uint16_t)(((uint32_t)lba << 4) |
                    (uint32_t)(offset / FAT12_DIR_ENTRY_SIZE));
}

static uint16_t fsck_location_lba(uint16_t location) {
  return location >> 4;
}

static uint16_t fsck_location_offset(uint16_t location) {
  return (location & 0x0Fu) * FAT12_DIR_ENTRY_SIZE;
}

static fat12_err_t fsck_stage_namespace(fsck_ctx_t *ctx, uint16_t lba,
                                        const uint8_t *sector,
                                        bool *staged) {
  fat12_err_t err = fat12_write_batch_add(
      &ctx->fat->namespace_batch, lba, sector);
  if (err == FAT12_ERR_FULL) {
    ctx->namespace_full = true;
    *staged = false;
    return FAT12_OK;
  }
  *staged = err == FAT12_OK;
  return err;
}

static fat12_err_t fsck_plan_fat(fsck_ctx_t *ctx, uint16_t cluster,
                                 uint16_t value) {
  if (ctx->mode != FSCK_REPAIR_FAT || !fsck_owner_valid(cluster)) {
    return FAT12_OK;
  }
  uint16_t current;
  fat12_err_t err = fat12_get_entry_batched(ctx->fat, cluster, &current);
  if (err != FAT12_OK) return err;
  if (current == value) return FAT12_OK;
  err = fat12_set_entry(ctx->fat, cluster, value);
  if (err == FAT12_OK && value == 0) ctx->freed++;
  return err;
}

static fat12_err_t fsck_plan_dir(fsck_ctx_t *ctx, uint16_t lba,
                                 uint16_t offset, uint16_t start_cluster,
                                 uint32_t size) {
  if (ctx->mode != FSCK_REPAIR_DIR) return FAT12_OK;
  uint8_t sector[DISK_SECTOR_SIZE];
  fat12_err_t err = fat12_read_sector_batched(ctx->fat, lba, sector);
  if (err != FAT12_OK) return err;
  fat12_dirent_t entry;
  fat12_decode_dirent(&entry, sector + offset);
  if (entry.start_cluster == start_cluster && entry.size == size) {
    return FAT12_OK;
  }
  entry.start_cluster = start_cluster;
  entry.size = size;
  fat12_encode_dirent(sector + offset, &entry);
  bool staged;
  err = fsck_stage_namespace(ctx, lba, sector, &staged);
  if (err == FAT12_OK && staged) ctx->truncated++;
  return err;
}

static fat12_err_t fsck_remove_entry(fsck_ctx_t *ctx, uint16_t lba,
                                     uint16_t offset, bool directory,
                                     bool duplicate) {
  if (ctx->mode != FSCK_REPAIR_DIR) return FAT12_OK;
  uint8_t sector[DISK_SECTOR_SIZE];
  fat12_err_t err = fat12_read_sector_batched(ctx->fat, lba, sector);
  if (err != FAT12_OK) return err;
  if (sector[offset] == FAT12_DIRENT_FREE) return FAT12_OK;
  sector[offset] = FAT12_DIRENT_FREE;
  bool staged;
  err = fsck_stage_namespace(ctx, lba, sector, &staged);
  if (err == FAT12_OK && staged) {
    if (directory) ctx->removed++;
    if (duplicate) ctx->duplicates_removed++;
  }
  return err;
}

static fat12_err_t fsck_remove_dir(fsck_ctx_t *ctx, uint16_t lba,
                                   uint16_t offset) {
  return fsck_remove_entry(ctx, lba, offset, true, false);
}

static uint16_t fsck_new_owner(fsck_ctx_t *ctx) {
  uint16_t owner = ctx->next_owner++;
  if (owner == 0) {
    ctx->out->incomplete = true;
    owner = ctx->next_owner++;
  }
  return owner;
}

static bool fsck_chain_collision(fsck_ctx_t *ctx, uint16_t cluster,
                                 uint16_t owner) {
  uint16_t existing = ctx->fat->fsck_owner[cluster];
  if (existing == 0) return false;
  if (existing == owner) {
    ctx->out->loops++;
    ctx->out->broken_chains++;
  } else {
    ctx->out->crosslinked++;
  }
  return true;
}

static fat12_err_t fsck_walk_file(fsck_ctx_t *ctx,
                                  const fat12_dirent_t *entry,
                                  uint16_t lba, uint16_t offset) {
  uint32_t declared = entry->size / DISK_SECTOR_SIZE +
      (entry->size % DISK_SECTOR_SIZE != 0);
  bool oversized = declared > FAT12_DATA_CLUSTERS;
  uint16_t expected = oversized ? FAT12_DATA_CLUSTERS : (uint16_t)declared;
  if (entry->size == 0) {
    if (entry->start_cluster != 0) {
      ctx->out->size_mismatches++;
      return fsck_plan_dir(ctx, lba, offset, 0, 0);
    }
    return FAT12_OK;
  }
  if (!fsck_owner_valid(entry->start_cluster)) {
    ctx->out->broken_chains++;
    ctx->out->size_mismatches++;
    return fsck_plan_dir(ctx, lba, offset, 0, 0);
  }

  uint16_t owner = fsck_new_owner(ctx);
  uint16_t cluster = entry->start_cluster;
  uint16_t previous = 0;
  uint16_t count = 0;
  while (count < expected) {
    if (!fsck_owner_valid(cluster)) {
      ctx->out->broken_chains++;
      ctx->out->size_mismatches++;
      return fsck_plan_dir(ctx, lba, offset, entry->start_cluster,
                           count * DISK_SECTOR_SIZE);
    }
    if (fsck_chain_collision(ctx, cluster, owner)) {
      ctx->out->size_mismatches++;
      fat12_err_t err = previous == 0
          ? FAT12_OK
          : fsck_plan_fat(ctx, previous, FAT12_EOC);
      if (err != FAT12_OK) return err;
      return fsck_plan_dir(ctx, lba, offset,
                           previous == 0 ? 0 : entry->start_cluster,
                           (uint32_t)count * DISK_SECTOR_SIZE);
    }
    ctx->fat->fsck_owner[cluster] = owner;
    count++;

    uint16_t next;
    fat12_err_t err = fsck_get_entry(ctx, cluster, &next);
    if (err != FAT12_OK) return err;
    if (fat12_is_bad(next)) {
      ctx->fat->fsck_owner[cluster] = 0;
      ctx->out->broken_chains++;
      ctx->out->size_mismatches++;
      if (previous != 0) {
        err = fsck_plan_fat(ctx, previous, FAT12_EOC);
        if (err != FAT12_OK) return err;
      }
      return fsck_plan_dir(ctx, lba, offset,
                           previous == 0 ? 0 : entry->start_cluster,
                           (uint32_t)(count - 1u) * DISK_SECTOR_SIZE);
    }
    if (count == expected) {
      if (oversized) {
        ctx->out->broken_chains++;
        ctx->out->size_mismatches++;
        if (!fat12_is_eof(next)) {
          err = fsck_plan_fat(ctx, cluster, FAT12_EOC);
          if (err != FAT12_OK) return err;
          if (ctx->mode == FSCK_REPAIR_FAT) ctx->tail_cuts++;
        }
        return fsck_plan_dir(ctx, lba, offset, entry->start_cluster,
                             (uint32_t)count * DISK_SECTOR_SIZE);
      }
      if (!fat12_is_eof(next)) {
        ctx->out->size_mismatches++;
        if (fsck_owner_valid(next) &&
            ctx->fat->fsck_owner[next] == owner) {
          ctx->out->loops++;
          ctx->out->broken_chains++;
        } else if (fsck_owner_valid(next) &&
                   ctx->fat->fsck_owner[next] != 0) {
          ctx->out->crosslinked++;
        }
        err = fsck_plan_fat(ctx, cluster, FAT12_EOC);
        if (err != FAT12_OK) return err;
        if (ctx->mode == FSCK_REPAIR_FAT) ctx->tail_cuts++;
      }
      return FAT12_OK;
    }
    if (fat12_is_eof(next)) {
      ctx->out->broken_chains++;
      ctx->out->size_mismatches++;
      return fsck_plan_dir(ctx, lba, offset, entry->start_cluster,
                           count * DISK_SECTOR_SIZE);
    }
    if (!fsck_owner_valid(next) || next >= FAT12_RESERVED_MIN) {
      ctx->out->broken_chains++;
      ctx->out->size_mismatches++;
      err = fsck_plan_fat(ctx, cluster, FAT12_EOC);
      if (err != FAT12_OK) return err;
      return fsck_plan_dir(ctx, lba, offset, entry->start_cluster,
                           count * DISK_SECTOR_SIZE);
    }
    previous = cluster;
    cluster = next;
  }
  return FAT12_OK;
}

static fat12_err_t fsck_read_location(fsck_ctx_t *ctx, uint16_t location,
                                      fat12_dirent_t *entry) {
  uint16_t lba = fsck_location_lba(location);
  uint16_t offset = fsck_location_offset(location);
  uint8_t sector[DISK_SECTOR_SIZE];
  fat12_err_t err = fat12_read_sector_batched(ctx->fat, lba, sector);
  if (err != FAT12_OK) return err;
  fat12_decode_dirent(entry, sector + offset);
  return FAT12_OK;
}

static fat12_err_t fsck_directory_queued(fsck_ctx_t *ctx, uint16_t cluster,
                                         bool *queued) {
  *queued = false;
  for (uint16_t index = 0; index < ctx->directory_count; index++) {
    fat12_dirent_t entry;
    fat12_err_t err = fsck_read_location(
        ctx, ctx->fat->fsck_directories[index], &entry);
    if (err != FAT12_OK) return err;
    if (entry.start_cluster == cluster) {
      *queued = true;
      return FAT12_OK;
    }
  }
  return FAT12_OK;
}

static fat12_err_t fsck_queue_directory(fsck_ctx_t *ctx, uint16_t cluster,
                                        uint16_t lba, uint16_t offset) {
  if (!fsck_owner_valid(cluster)) {
    ctx->out->broken_chains++;
    return fsck_remove_dir(ctx, lba, offset);
  }
  bool queued;
  fat12_err_t err = fsck_directory_queued(ctx, cluster, &queued);
  if (err != FAT12_OK) return err;
  if (queued) {
    ctx->out->crosslinked++;
    return fsck_remove_dir(ctx, lba, offset);
  }
  if (ctx->directory_count >= FAT12_DATA_CLUSTERS) {
    ctx->out->incomplete = true;
    return FAT12_OK;
  }
  ctx->fat->fsck_directories[ctx->directory_count++] =
      fsck_location(lba, offset);
  return FAT12_OK;
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

static fat12_err_t fsck_duplicate_in_sector(
    fsck_ctx_t *ctx, uint16_t lba, uint16_t current_lba,
    uint16_t current_offset, const fat12_dirent_t *entry, bool *duplicate,
    bool *reached) {
  uint8_t sector[DISK_SECTOR_SIZE];
  fat12_err_t err = ctx->fat->batch.active
      ? fat12_read_sector_batched(ctx->fat, lba, sector)
      : fat12_read_sector(ctx->fat, lba, sector);
  if (err != FAT12_OK) return err;
  for (uint16_t offset = 0; offset < DISK_SECTOR_SIZE;
       offset += FAT12_DIR_ENTRY_SIZE) {
    if (lba == current_lba && offset == current_offset) {
      *reached = true;
      return FAT12_OK;
    }
    fat12_dirent_t candidate;
    fat12_decode_dirent(&candidate, sector + offset);
    if (fat12_entry_is_end(&candidate)) return FAT12_OK;
    if (fsck_namespace_entry(&candidate) &&
        fsck_same_name(&candidate, entry)) {
      *duplicate = true;
      return FAT12_OK;
    }
  }
  return FAT12_OK;
}

static fat12_err_t fsck_name_duplicate(
    fsck_ctx_t *ctx, uint16_t directory_start, uint16_t current_lba,
    uint16_t current_offset, const fat12_dirent_t *entry, bool *duplicate) {
  *duplicate = false;
  bool reached = false;
  if (directory_start == 0) {
    for (uint16_t sector = 0;
         sector < FAT12_ROOT_DIR_SECTORS && !reached && !*duplicate; sector++) {
      uint16_t lba = FAT12_ROOT_START + sector;
      fat12_err_t err = fsck_duplicate_in_sector(
          ctx, lba, current_lba, current_offset, entry, duplicate, &reached);
      if (err != FAT12_OK) return err;
    }
    return reached || *duplicate ? FAT12_OK : FAT12_ERR_CORRUPT;
  }

  uint16_t cluster = directory_start;
  for (uint16_t count = 0;
       count < FAT12_DATA_CLUSTERS && !reached && !*duplicate; count++) {
    if (!fat12_cluster_valid(cluster)) return FAT12_ERR_CORRUPT;
    uint16_t lba = fat12_cluster_to_lba(cluster);
    fat12_err_t err = fsck_duplicate_in_sector(
        ctx, lba, current_lba, current_offset, entry, duplicate, &reached);
    if (err != FAT12_OK || reached || *duplicate) return err;
    uint16_t next;
    err = fsck_get_entry(ctx, cluster, &next);
    if (err != FAT12_OK) return err;
    if (fat12_is_eof(next)) break;
    if (!fat12_cluster_valid(next)) return FAT12_ERR_CORRUPT;
    cluster = next;
  }
  return reached ? FAT12_OK : FAT12_ERR_CORRUPT;
}

static fat12_err_t fsck_scan_dirent(fsck_ctx_t *ctx, const uint8_t *raw,
                                    uint16_t lba, uint16_t offset,
                                    uint16_t directory_start, bool *end) {
  fat12_dirent_t entry;
  fat12_decode_dirent(&entry, raw);
  if (fat12_entry_is_end(&entry)) {
    *end = true;
    return FAT12_OK;
  }
  if (!fsck_namespace_entry(&entry)) return FAT12_OK;
  bool duplicate;
  fat12_err_t err = fsck_name_duplicate(
      ctx, directory_start, lba, offset, &entry, &duplicate);
  if (err != FAT12_OK) return err;
  if (duplicate) {
    bool directory = (entry.attr & FAT12_ATTR_DIRECTORY) != 0;
    ctx->out->duplicate_names++;
    if (ctx->mode == FSCK_REPAIR_DIR) {
      return fsck_remove_entry(ctx, lba, offset, directory, true);
    }
  }
  if ((entry.attr & FAT12_ATTR_DIRECTORY) != 0) {
    ctx->out->directories++;
    return fsck_queue_directory(ctx, entry.start_cluster, lba, offset);
  }
  ctx->out->files++;
  return fsck_walk_file(ctx, &entry, lba, offset);
}

static fat12_err_t fsck_walk_directory(fsck_ctx_t *ctx, uint16_t start,
                                       uint16_t location) {
  uint16_t owner = fsck_new_owner(ctx);
  uint16_t cluster = start;
  uint16_t previous = 0;
  bool end = false;
  for (uint16_t count = 0; count < FAT12_DATA_CLUSTERS; count++) {
    if (!fsck_owner_valid(cluster)) {
      ctx->out->broken_chains++;
      return FAT12_OK;
    }
    if (fsck_chain_collision(ctx, cluster, owner)) {
      return fsck_remove_dir(ctx, fsck_location_lba(location),
                             fsck_location_offset(location));
    }
    ctx->fat->fsck_owner[cluster] = owner;

    uint16_t next;
    fat12_err_t err = fsck_get_entry(ctx, cluster, &next);
    if (err != FAT12_OK) return err;
    if (fat12_is_bad(next)) {
      ctx->fat->fsck_owner[cluster] = 0;
      ctx->out->broken_chains++;
      if (previous == 0) {
        return fsck_remove_dir(ctx, fsck_location_lba(location),
                               fsck_location_offset(location));
      }
      return fsck_plan_fat(ctx, previous, FAT12_EOC);
    }

    if (!end) {
      uint16_t lba = fat12_cluster_to_lba(cluster);
      uint8_t sector[DISK_SECTOR_SIZE];
      err = ctx->fat->batch.active
          ? fat12_read_sector_batched(ctx->fat, lba, sector)
          : fat12_read_sector(ctx->fat, lba, sector);
      if (err != FAT12_OK) return err;
      for (uint16_t offset = 0; offset < DISK_SECTOR_SIZE && !end;
           offset += FAT12_DIR_ENTRY_SIZE) {
        err = fsck_scan_dirent(
            ctx, sector + offset, lba, offset, start, &end);
        if (err != FAT12_OK) return err;
      }
    }

    if (fat12_is_eof(next)) return FAT12_OK;
    if (!fsck_owner_valid(next) || next >= FAT12_RESERVED_MIN) {
      ctx->out->broken_chains++;
      return fsck_plan_fat(ctx, cluster, FAT12_EOC);
    }
    if (ctx->fat->fsck_owner[next] != 0) {
      if (ctx->fat->fsck_owner[next] == owner) {
        ctx->out->loops++;
        ctx->out->broken_chains++;
      } else {
        ctx->out->crosslinked++;
      }
      return fsck_plan_fat(ctx, cluster, FAT12_EOC);
    }
    previous = cluster;
    cluster = next;
  }
  ctx->out->loops++;
  ctx->out->broken_chains++;
  return FAT12_OK;
}

static fat12_err_t fsck_compare_fats(fat12_t *fat,
                                     fat12_mirrors_t *mirrors) {
  uint8_t primary[DISK_SECTOR_SIZE];
  uint8_t copy[DISK_SECTOR_SIZE];
  memset(mirrors, 0, sizeof(*mirrors));
  for (uint16_t sector = 0; sector < FAT12_SECTORS_PER_FAT; sector++) {
    fat12_err_t err = fat12_read_sector(
        fat, (uint16_t)(FAT12_FAT1_START + sector), primary);
    if (err != FAT12_OK) return err;
    if (sector == 0) {
      mirrors->valid[0] = primary[0] == FAT12_MEDIA_DESCRIPTOR &&
          primary[1] == 0xFF && primary[2] == 0xFF;
    }
    for (uint8_t index = 1; index < FAT12_NUM_FATS; index++) {
      uint16_t lba = (uint16_t)(FAT12_FAT1_START +
          (uint16_t)index * FAT12_SECTORS_PER_FAT + sector);
      err = fat12_read_sector(fat, lba, copy);
      if (err != FAT12_OK) return err;
      if (sector == 0) {
        mirrors->valid[index] = copy[0] == FAT12_MEDIA_DESCRIPTOR &&
            copy[1] == 0xFF && copy[2] == 0xFF;
      }
      if (memcmp(primary, copy, DISK_SECTOR_SIZE) != 0) {
        mirrors->mismatch = true;
      }
    }
  }
  fat->fat_mismatch = mirrors->mismatch;
  fat->fat_markers_invalid = false;
  for (uint8_t index = 0; index < FAT12_NUM_FATS; index++) {
    fat->fat_markers_invalid |= !mirrors->valid[index];
  }
  return FAT12_OK;
}

static void fsck_reset(fsck_ctx_t *ctx, fat12_t *fat, fat12_fsck_t *out,
                       fsck_mode_t mode, uint16_t fat_start) {
  memset(fat->fsck_owner, 0, sizeof(fat->fsck_owner));
  ctx->fat = fat;
  ctx->out = out;
  ctx->mode = mode;
  ctx->directory_count = 0;
  ctx->directory_cursor = 0;
  ctx->next_owner = 1;
  ctx->freed = 0;
  ctx->truncated = 0;
  ctx->removed = 0;
  ctx->duplicates_removed = 0;
  ctx->tail_cuts = 0;
  ctx->fat_start = fat_start;
  ctx->namespace_full = false;
}

static fat12_err_t fsck_scan(fsck_ctx_t *ctx) {
  fat12_err_t err = FAT12_OK;

  bool end = false;
  for (uint16_t sector_index = 0;
       sector_index < FAT12_ROOT_DIR_SECTORS && !end; sector_index++) {
    uint16_t lba = FAT12_ROOT_START + sector_index;
    uint8_t sector[DISK_SECTOR_SIZE];
    err = ctx->fat->batch.active
        ? fat12_read_sector_batched(ctx->fat, lba, sector)
        : fat12_read_sector(ctx->fat, lba, sector);
    if (err != FAT12_OK) return err;
    for (uint16_t offset = 0; offset < DISK_SECTOR_SIZE && !end;
         offset += FAT12_DIR_ENTRY_SIZE) {
      err = fsck_scan_dirent(ctx, sector + offset, lba, offset, 0, &end);
      if (err != FAT12_OK) return err;
    }
  }

  while (ctx->directory_cursor < ctx->directory_count) {
    uint16_t location =
        ctx->fat->fsck_directories[ctx->directory_cursor++];
    fat12_dirent_t entry;
    err = fsck_read_location(ctx, location, &entry);
    if (err != FAT12_OK) return err;
    if (!fat12_entry_valid(&entry) ||
        (entry.attr & FAT12_ATTR_DIRECTORY) == 0) continue;
    err = fsck_walk_directory(ctx, entry.start_cluster, location);
    if (err != FAT12_OK) return err;
  }

  fat12_scan_t scan;
  fat12_scan_init(&scan, ctx->fat, ctx->fat_start,
                  ctx->fat->batch.active, 2);
  uint16_t cluster;
  uint16_t entry;
  while ((err = fat12_scan_next(
              &scan, FAT12_CLUSTER_LIMIT,
              &cluster, &entry)) == FAT12_OK) {
    if (entry == 0 || fat12_is_bad(entry)) continue;
    if (ctx->fat->fsck_owner[cluster] != 0) continue;
    ctx->out->lost_clusters++;
    err = fsck_plan_fat(ctx, cluster, 0);
    if (err != FAT12_OK) return err;
  }
  return err == FAT12_ERR_EOF ? FAT12_OK : err;
}

static void fsck_capture_reachable(fat12_t *fat) {
  memset(fat->fsck_reachable, 0, sizeof(fat->fsck_reachable));
  for (uint16_t cluster = 2; cluster < FAT12_CLUSTER_LIMIT; cluster++) {
    if (fat->fsck_owner[cluster] != 0) {
      fat->fsck_reachable[cluster / 8u] |= (uint8_t)(1u << (cluster % 8u));
    }
  }
}

static bool fsck_was_reachable(const fat12_t *fat, uint16_t cluster) {
  return (fat->fsck_reachable[cluster / 8u] &
          (1u << (cluster % 8u))) != 0;
}

static uint32_t fsck_structural_score(const fat12_fsck_t *report) {
  uint32_t score = 0;
  score += report->crosslinked;
  score += report->loops;
  score += report->broken_chains;
  score += report->size_mismatches;
  score += report->incomplete ? 1u : 0u;
  return score;
}

static uint32_t fsck_candidate_score(const fat12_fsck_t *report,
                                     bool marker_valid) {
  return fsck_structural_score(report) + (marker_valid ? 0u : 1u);
}

static fat12_err_t fsck_reachable_equal(fat12_t *fat, bool *equal) {
  *equal = true;
  for (uint16_t cluster = 2; cluster < FAT12_CLUSTER_LIMIT; cluster++) {
    bool primary = fsck_was_reachable(fat, cluster);
    bool secondary = fat->fsck_owner[cluster] != 0;
    if (primary != secondary) {
      *equal = false;
      return FAT12_OK;
    }
    if (!primary) continue;
    uint16_t first;
    uint16_t second;
    fat12_err_t err = fat12_resolve_entry(
        fat, FAT12_FAT1_START, cluster, false, &first);
    if (err != FAT12_OK) return err;
    err = fat12_resolve_entry(
        fat, FAT12_FAT2_START, cluster, false, &second);
    if (err != FAT12_OK) return err;
    if (first != second && !(fat12_is_eof(first) && fat12_is_eof(second))) {
      *equal = false;
      return FAT12_OK;
    }
  }
  return FAT12_OK;
}

static fat12_err_t fsck_select_fat(fat12_t *fat,
                                   const fat12_mirrors_t *mirrors,
                                   fat12_fsck_t *out,
                                   uint16_t *fat_start) {
  fsck_ctx_t ctx;
  fat12_fsck_t primary;
  memset(&primary, 0, sizeof(primary));
  fsck_reset(&ctx, fat, &primary, FSCK_REPORT, FAT12_FAT1_START);
  fat12_err_t err = fsck_scan(&ctx);
  if (err != FAT12_OK) return err;
  fsck_capture_reachable(fat);
  uint32_t primary_score = fsck_candidate_score(&primary, mirrors->valid[0]);

  if (!mirrors->mismatch) {
    *out = primary;
    out->fat1_score = primary_score;
    out->fat2_score = primary_score;
    out->authoritative_fat = 1;
    *fat_start = FAT12_FAT1_START;
    return FAT12_OK;
  }

  fat12_fsck_t secondary;
  memset(&secondary, 0, sizeof(secondary));
  fsck_reset(&ctx, fat, &secondary, FSCK_REPORT, FAT12_FAT2_START);
  err = fsck_scan(&ctx);
  if (err != FAT12_OK) return err;
  uint32_t secondary_score = fsck_candidate_score(&secondary, mirrors->valid[1]);
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
    if (err != FAT12_OK) return err;
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
      if (err != FAT12_OK) return err;
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
    return FAT12_ERR_AMBIGUOUS;
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
  return FAT12_OK;
}

static fat12_err_t fsck_stage_fat_copy(fat12_t *fat, uint16_t source,
                                       uint16_t target) {
  for (uint16_t index = 0; index < FAT12_SECTORS_PER_FAT; index++) {
    uint8_t sector[DISK_SECTOR_SIZE];
    fat12_err_t err = fat12_read_sector(fat, source + index, sector);
    if (err != FAT12_OK) return err;
    err = fat12_write_sector_batched(fat, target + index, sector);
    if (err != FAT12_OK) return err;
  }
  return FAT12_OK;
}

static fat12_err_t fsck_stage_markers(fat12_t *fat) {
  for (uint8_t copy = 0; copy < FAT12_NUM_FATS; copy++) {
    uint16_t lba = (uint16_t)(FAT12_FAT1_START +
        (uint16_t)copy * FAT12_SECTORS_PER_FAT);
    uint8_t sector[DISK_SECTOR_SIZE];
    fat12_err_t err = fat12_read_sector_batched(fat, lba, sector);
    if (err != FAT12_OK) return err;
    sector[0] = FAT12_MEDIA_DESCRIPTOR;
    sector[1] = 0xFF;
    sector[2] = 0xFF;
    err = fat12_write_sector_batched(fat, lba, sector);
    if (err != FAT12_OK) return err;
  }
  return FAT12_OK;
}

static fat12_err_t fsck_preserve_bad_union(fat12_t *fat) {
  for (uint16_t cluster = 2; cluster < FAT12_CLUSTER_LIMIT; cluster++) {
    if (fsck_was_reachable(fat, cluster)) continue;
    uint16_t first;
    uint16_t second;
    fat12_err_t err = fat12_resolve_entry(
        fat, FAT12_FAT1_START, cluster, false, &first);
    if (err != FAT12_OK) return err;
    err = fat12_resolve_entry(
        fat, FAT12_FAT2_START, cluster, false, &second);
    if (err != FAT12_OK) return err;
    if (fat12_is_bad(first) || fat12_is_bad(second)) {
      err = fat12_set_entry(fat, cluster, FAT12_BAD);
      if (err != FAT12_OK) return err;
    }
  }
  return FAT12_OK;
}

fat12_err_t fat12_fsck(fat12_t *fat, fat12_fsck_t *out, bool repair) {
  if (fat == NULL || out == NULL) return FAT12_ERR_INVALID;
  if (fat12_busy(fat)) return FAT12_ERR_BUSY;
  if (repair && fat->io.write == NULL) return FAT12_ERR_READ_ONLY;
  memset(out, 0, sizeof(*out));
  fat12_mirrors_t mirrors;
  fat12_err_t err = fsck_compare_fats(fat, &mirrors);
  if (err != FAT12_OK) return err;
  uint16_t selected_fat;
  err = fsck_select_fat(fat, &mirrors, out, &selected_fat);
  if (err != FAT12_OK) return err;
  fat->fat_start = selected_fat;
  out->fat_mismatch = mirrors.mismatch;
  out->fat_markers_invalid = false;
  for (uint8_t index = 0; index < FAT12_NUM_FATS; index++) {
    out->fat_markers_invalid |= !mirrors.valid[index];
  }
  fat->fat_markers_invalid = out->fat_markers_invalid;
  if (!repair || out->incomplete) return FAT12_OK;

  if (!fat12_batch_begin(fat)) return FAT12_ERR_BUSY;
  fsck_ctx_t ctx;
  fat12_fsck_t pass;
  memset(&pass, 0, sizeof(pass));
  fsck_reset(&ctx, fat, &pass, FSCK_REPAIR_DIR, selected_fat);
  err = fsck_scan(&ctx);
  out->truncated_files = ctx.truncated;
  out->removed_directories = ctx.removed;
  out->removed_duplicates = ctx.duplicates_removed;
  if (err != FAT12_OK) {
    fat12_batch_abort(fat);
    return err;
  }
  if (ctx.namespace_full) {
    err = fat12_write_metadata_flush(fat, &fat->namespace_batch);
    if (err == FAT12_OK) out->repair_pending = true;
    fat12_batch_abort(fat);
    return err;
  }

  memset(&pass, 0, sizeof(pass));
  fsck_reset(&ctx, fat, &pass, FSCK_REPORT, selected_fat);
  err = fsck_scan(&ctx);
  if (err == FAT12_OK) fsck_capture_reachable(fat);

  bool repaired_fat1 = !mirrors.valid[0];
  bool repaired_fat2 = !mirrors.valid[1];
  if (err == FAT12_OK && mirrors.mismatch) {
    uint16_t target = selected_fat == FAT12_FAT1_START
        ? FAT12_FAT2_START : FAT12_FAT1_START;
    err = fsck_stage_fat_copy(fat, selected_fat, target);
    repaired_fat1 |= target == FAT12_FAT1_START;
    repaired_fat2 |= target == FAT12_FAT2_START;
  }
  if (err == FAT12_OK && out->fat_markers_invalid) {
    err = fsck_stage_markers(fat);
  }
  if (err == FAT12_OK && mirrors.mismatch) {
    err = fsck_preserve_bad_union(fat);
  }

  memset(&pass, 0, sizeof(pass));
  if (err == FAT12_OK) {
    fsck_reset(&ctx, fat, &pass, FSCK_REPAIR_FAT, selected_fat);
    err = fsck_scan(&ctx);
  }
  if (err == FAT12_OK) {
    out->freed = ctx.freed;
    out->freed_tails = ctx.tail_cuts;
  }
  if (err == FAT12_OK) {
    err = fat12_write_metadata_flush(fat, &fat->namespace_batch);
  }
  if (err == FAT12_OK) err = fat12_write_batch_flush(fat);
  if (err == FAT12_OK) {
    out->repaired_fat1 = repaired_fat1;
    out->repaired_fat2 = repaired_fat2;
    fat->fat_start = FAT12_FAT1_START;
    fat->fat_mismatch = false;
    fat->fat_markers_invalid = false;
  }
  fat12_batch_abort(fat);
  return err;
}

static void fat12_build_boot_sector(uint8_t *boot, const char *volume_label) {
  memset(boot, 0, DISK_SECTOR_SIZE);
  boot[0] = 0xEB;
  boot[1] = 0x3C;
  boot[2] = 0x90;
  memcpy(boot + 3, "MSDOS5.0", 8);
  fat12_encode_bpb(boot);
  boot[38] = 0x29;
  boot[39] = 0x12;
  boot[40] = 0x34;
  boot[41] = 0x56;
  boot[42] = 0x78;
  if (volume_label != NULL) {
    size_t index = 0;
    while (index < 11 && volume_label[index] != '\0') {
      boot[43 + index] = (uint8_t)fat12_toupper(volume_label[index]);
      index++;
    }
    while (index < 11) boot[43 + index++] = ' ';
  } else {
    memcpy(boot + 43, "NO NAME    ", 11);
  }
  memcpy(boot + 54, "FAT12   ", 8);
  boot[FAT12_BOOT_SIG_OFFSET] = 0x55;
  boot[FAT12_BOOT_SIG_OFFSET + 1] = 0xAA;
}

static void fat12_build_volume_label(uint8_t *sector,
                                     const char *volume_label,
                                     size_t length) {
  memset(sector, 0, DISK_SECTOR_SIZE);
  if (volume_label == NULL) return;
  fat12_dirent_t entry;
  memset(&entry, 0, sizeof(entry));
  memset(entry.name, ' ', sizeof(entry.name));
  memset(entry.ext, ' ', sizeof(entry.ext));
  size_t name_length = length < sizeof(entry.name) ? length : sizeof(entry.name);
  for (size_t index = 0; index < name_length; index++) {
    entry.name[index] = fat12_toupper(volume_label[index]);
  }
  for (size_t index = sizeof(entry.name);
       index < length && index < sizeof(entry.name) + sizeof(entry.ext); index++) {
    entry.ext[index - sizeof(entry.name)] = fat12_toupper(volume_label[index]);
  }
  entry.attr = FAT12_ATTR_VOLUME_ID;
  fat12_encode_dirent(sector, &entry);
}

static fat12_err_t fat12_volume_label_validate(const char *label,
                                               size_t *length) {
  *length = 0;
  if (label == NULL) return FAT12_OK;
  while (*length <= 11 && label[*length] != '\0') {
    unsigned char value = (unsigned char)label[*length];
    if (value < 0x20 || value > 0x7E ||
        strchr("\"*+,./:;<=>?[\\]|", value) != NULL) {
      return FAT12_ERR_INVALID;
    }
    (*length)++;
  }
  return *length > 11 || *length == 0 ? FAT12_ERR_INVALID : FAT12_OK;
}

static void fat12_fill_format_sector(uint8_t *sector, uint16_t lba,
                                     const uint8_t *boot,
                                     const uint8_t *fat_first,
                                     const uint8_t *root_first,
                                     const char *volume_label) {
  uint16_t fat1 = FAT12_RESERVED_SECTORS;
  uint16_t fat2 = fat1 + FAT12_SECTORS_PER_FAT;
  uint16_t root = fat2 + FAT12_SECTORS_PER_FAT;
  if (lba == 0) {
    memcpy(sector, boot, DISK_SECTOR_SIZE);
  } else if ((lba >= fat1 && lba < fat1 + FAT12_SECTORS_PER_FAT) ||
             (lba >= fat2 && lba < fat2 + FAT12_SECTORS_PER_FAT)) {
    uint16_t start = lba < fat2 ? fat1 : fat2;
    if (lba == start) memcpy(sector, fat_first, DISK_SECTOR_SIZE);
    else memset(sector, 0, DISK_SECTOR_SIZE);
  } else if (lba >= root && lba < root + FAT12_ROOT_DIR_SECTORS) {
    if (lba == root && volume_label != NULL) {
      memcpy(sector, root_first, DISK_SECTOR_SIZE);
    } else {
      memset(sector, 0, DISK_SECTOR_SIZE);
    }
  } else {
    memset(sector, 0, DISK_SECTOR_SIZE);
  }
}

fat12_err_t fat12_format(fat12_t *fat, fat12_io_t io,
                         const char *volume_label,
                         bool write_all_tracks,
                         fat12_progress_t progress,
                         void *progress_ctx) {
  if (fat == NULL || io.read == NULL || io.write == NULL) {
    return FAT12_ERR_INVALID;
  }
  size_t label_length;
  fat12_err_t err = fat12_volume_label_validate(volume_label, &label_length);
  if (err != FAT12_OK) return err;
  memset(fat, 0, sizeof(*fat));
  fat->io = io;
  fat->last_io = BLOCK_OK;
  fat->fat_start = FAT12_FAT1_START;
  uint8_t *boot = fat->batch.data[0];
  fat12_build_boot_sector(boot, volume_label);
  uint8_t *fat_first = fat->batch.data[1];
  memset(fat_first, 0, DISK_SECTOR_SIZE);
  fat_first[0] = FAT12_MEDIA_DESCRIPTOR;
  fat_first[1] = 0xFF;
  fat_first[2] = 0xFF;
  uint8_t *root_first = fat->batch.data[2];
  fat12_build_volume_label(root_first, volume_label, label_length);

  uint16_t tracks = (uint16_t)(write_all_tracks
      ? DISK_TRACK_COUNT
      : (FAT12_DATA_START + DISK_SECTORS_PER_TRACK - 1u) /
        DISK_SECTORS_PER_TRACK);
  for (uint16_t track_index = 0; track_index < tracks; track_index++) {
    track_t *track = &fat->write_track;
    memset(track, 0, sizeof(*track));
    track->cylinder = (uint8_t)(track_index / DISK_HEADS);
    track->head = (uint8_t)(track_index % DISK_HEADS);
    for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
      uint16_t lba = (uint16_t)(
          track_index * DISK_SECTORS_PER_TRACK + sector);
      if (!write_all_tracks && lba >= FAT12_DATA_START) continue;
      fat12_fill_format_sector(
          track->data[sector], lba, boot, fat_first, root_first, volume_label);
      if (!track_mark(track, sector)) return FAT12_ERR_INVALID;
    }
    err = fat12_write_track(fat, track);
    if (err != FAT12_OK) return err;
    if (progress != NULL) {
      progress(progress_ctx, track->cylinder, track->head,
               (uint16_t)(track_index + 1u), tracks);
    }
  }
  return FAT12_OK;
}
