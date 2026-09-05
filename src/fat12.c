#include "internal/fat12_internal.h"
#include "internal/byteorder.h"
#include <string.h>

static bool fat12_bpb_valid(const uint8_t *boot) {
  const uint8_t *p = boot + FAT12_BPB_OFFSET;
  uint32_t fat_entries =
      (FAT12_SECTORS_PER_FAT * DISK_SECTOR_SIZE * 2u) / 3u;
  return load_le16(p + 0) == DISK_SECTOR_SIZE &&
         p[2] == FAT12_SECTORS_PER_CLUSTER &&
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
  p[2] = FAT12_SECTORS_PER_CLUSTER;
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

disk_err_t fat12_init(fat12_t *fat, cache_t *cache) {
  if (fat == NULL || cache == NULL) return DISK_ERR_INVALID;
  memset(fat, 0, sizeof(*fat));
  fat->cache = cache;
  fat->fat_start = FAT12_FAT1_START;

  uint8_t boot[DISK_SECTOR_SIZE];
  disk_err_t err = cache_read(fat->cache, 0, boot);
  if (err != DISK_OK) return err;
  if (boot[FAT12_BOOT_SIG_OFFSET] != 0x55 ||
      boot[FAT12_BOOT_SIG_OFFSET + 1] != 0xAA) {
    return DISK_ERR_INVALID;
  }
  if (!fat12_bpb_valid(boot)) return DISK_ERR_INVALID;

  return fat12_select_active_fat(fat);
}

static disk_err_t fat12_volume_label_validate(const char *label,
                                              size_t *length) {
  *length = 0;
  if (label == NULL) return DISK_OK;
  while (*length <= 11 && label[*length] != '\0') {
    unsigned char value = (unsigned char)label[*length];
    if (value < 0x20 || value > 0x7E ||
        strchr("\"*+,./:;<=>?[\\]|", value) != NULL) {
      return DISK_ERR_INVALID;
    }
    (*length)++;
  }
  return *length > 11 || *length == 0 ? DISK_ERR_INVALID : DISK_OK;
}

static void fat12_format_sector(uint8_t *sector, uint16_t lba,
                                const char *label, size_t label_length) {
  memset(sector, 0, DISK_SECTOR_SIZE);
  if (lba == 0) {
    sector[0] = 0xEB;
    sector[1] = 0x3C;
    sector[2] = 0x90;
    memcpy(sector + 3, "MSDOS5.0", 8);
    fat12_encode_bpb(sector);
    sector[38] = 0x29;
    sector[39] = 0x12;
    sector[40] = 0x34;
    sector[41] = 0x56;
    sector[42] = 0x78;
    memset(sector + 43, ' ', 11);
    if (label != NULL) {
      for (size_t index = 0; index < label_length; index++) {
        sector[43 + index] = (uint8_t)fat12_toupper(label[index]);
      }
    } else {
      memcpy(sector + 43, "NO NAME", 7);
    }
    memcpy(sector + 54, "FAT12   ", 8);
    sector[FAT12_BOOT_SIG_OFFSET] = 0x55;
    sector[FAT12_BOOT_SIG_OFFSET + 1] = 0xAA;
    return;
  }
  if (lba == FAT12_FAT1_START || lba == FAT12_FAT2_START) {
    sector[0] = FAT12_MEDIA_DESCRIPTOR;
    sector[1] = 0xFF;
    sector[2] = 0xFF;
    return;
  }
  if (lba == FAT12_ROOT_START && label != NULL) {
    fat12_dirent_t entry;
    memset(&entry, 0, sizeof(entry));
    memset(entry.name, ' ', sizeof(entry.name));
    memset(entry.ext, ' ', sizeof(entry.ext));
    for (size_t index = 0; index < label_length; index++) {
      char value = fat12_toupper(label[index]);
      if (index < sizeof(entry.name)) entry.name[index] = value;
      else entry.ext[index - sizeof(entry.name)] = value;
    }
    entry.attr = FAT12_ATTR_VOLUME_ID;
    fat12_encode_dirent(sector, &entry);
  }
}

disk_err_t fat12_format(fat12_t *fat, cache_t *cache, const char *volume_label,
                        bool write_all_tracks, fat12_progress_t progress,
                        void *progress_ctx) {
  if (fat == NULL || cache == NULL) return DISK_ERR_INVALID;
  size_t label_length;
  disk_err_t err = fat12_volume_label_validate(volume_label, &label_length);
  if (err != DISK_OK) return err;
  err = cache_writable(cache);
  if (err != DISK_OK) return err;
  memset(fat, 0, sizeof(*fat));
  fat->cache = cache;
  fat->fat_start = FAT12_FAT1_START;

  uint16_t tracks = (uint16_t)(write_all_tracks
      ? DISK_TRACK_COUNT
      : (FAT12_DATA_START + DISK_SECTORS_PER_TRACK - 1u) /
        DISK_SECTORS_PER_TRACK);
  uint8_t sector[DISK_SECTOR_SIZE];
  for (uint16_t track = 0; track < tracks; track++) {
    for (uint8_t index = 0; index < DISK_SECTORS_PER_TRACK; index++) {
      uint16_t lba = (uint16_t)(track * DISK_SECTORS_PER_TRACK + index);
      fat12_format_sector(sector, lba, volume_label, label_length);
      err = cache_write(cache, lba, sector);
      if (err != DISK_OK) return err;
    }
    err = cache_flush(cache);
    if (err != DISK_OK) return err;
    if (progress != NULL) {
      progress(progress_ctx, (uint8_t)(track / DISK_HEADS),
               (uint8_t)(track % DISK_HEADS), (uint16_t)(track + 1u), tracks);
    }
  }
  return DISK_OK;
}
