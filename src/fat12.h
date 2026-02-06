#ifndef FAT12_H
#define FAT12_H

#include <stdint.h>
#include <stdbool.h>
#include "floppy.h"

#define FAT12_DIR_ENTRY_SIZE 32
#define FAT12_FILENAME_LEN 8
#define FAT12_EXTENSION_LEN 3
#define FAT12_MAX_CLUSTER_SECTORS 1

typedef struct {
  bool (*read)(void *ctx, sector_t *sector);
  bool (*write)(void *ctx, track_t *track);
  void *ctx;
} fat12_io_t;

typedef struct {
  uint16_t bytes_per_sector;
  uint8_t  sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t  num_fats;
  uint16_t root_entries;
  uint16_t total_sectors;
  uint8_t  media_descriptor;
  uint16_t sectors_per_fat;
  uint16_t sectors_per_track;
  uint16_t num_heads;
  uint32_t hidden_sectors;
} fat12_bpb_t;

#define FAT12_BPB_OFFSET 11
#define FAT12_BOOT_SIG_OFFSET 510

typedef struct {
  uint16_t bytes_per_sector;
  uint8_t  sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t  num_fats;
  uint16_t root_entries;
  uint16_t total_sectors;
  uint8_t  media_descriptor;
  uint16_t sectors_per_fat;
  uint16_t sectors_per_track;
  uint16_t num_heads;
  uint32_t hidden_sectors;
} __attribute__((packed)) fat12_bpb_raw_t;

_Static_assert(sizeof(fat12_bpb_raw_t) == 21, "fat12_bpb_raw_t must match on-disk BPB layout");

static inline void fat12_bpb_from_raw(fat12_bpb_t *bpb, const fat12_bpb_raw_t *raw) {
  bpb->bytes_per_sector    = raw->bytes_per_sector;
  bpb->sectors_per_cluster = raw->sectors_per_cluster;
  bpb->reserved_sectors    = raw->reserved_sectors;
  bpb->num_fats            = raw->num_fats;
  bpb->root_entries        = raw->root_entries;
  bpb->total_sectors       = raw->total_sectors;
  bpb->media_descriptor    = raw->media_descriptor;
  bpb->sectors_per_fat     = raw->sectors_per_fat;
  bpb->sectors_per_track   = raw->sectors_per_track;
  bpb->num_heads           = raw->num_heads;
  bpb->hidden_sectors      = raw->hidden_sectors;
}

static inline void fat12_bpb_to_raw(fat12_bpb_raw_t *raw, const fat12_bpb_t *bpb) {
  raw->bytes_per_sector    = bpb->bytes_per_sector;
  raw->sectors_per_cluster = bpb->sectors_per_cluster;
  raw->reserved_sectors    = bpb->reserved_sectors;
  raw->num_fats            = bpb->num_fats;
  raw->root_entries        = bpb->root_entries;
  raw->total_sectors       = bpb->total_sectors;
  raw->media_descriptor    = bpb->media_descriptor;
  raw->sectors_per_fat     = bpb->sectors_per_fat;
  raw->sectors_per_track   = bpb->sectors_per_track;
  raw->num_heads           = bpb->num_heads;
  raw->hidden_sectors      = bpb->hidden_sectors;
}

typedef struct {
  char     name[FAT12_FILENAME_LEN];
  char     ext[FAT12_EXTENSION_LEN];
  uint8_t  attr;
  uint8_t  reserved[10];
  uint16_t time;
  uint16_t date;
  uint16_t start_cluster;
  uint32_t size;
} __attribute__((packed)) fat12_dirent_t;

#define FAT12_ATTR_READ_ONLY  0x01
#define FAT12_ATTR_HIDDEN     0x02
#define FAT12_ATTR_SYSTEM     0x04
#define FAT12_ATTR_VOLUME_ID  0x08
#define FAT12_ATTR_DIRECTORY  0x10
#define FAT12_ATTR_ARCHIVE    0x20
#define FAT12_ATTR_LFN        0x0F

typedef struct {
  fat12_io_t io;
  fat12_bpb_t bpb;

  uint16_t fat_start_sector;
  uint16_t root_dir_start_sector;
  uint16_t root_dir_sectors;
  uint16_t data_start_sector;
  uint16_t total_clusters;

  sector_t sector_buf;
} fat12_t;

typedef enum {
  FAT12_OK = 0,
  FAT12_ERR_READ,
  FAT12_ERR_WRITE,
  FAT12_ERR_INVALID,
  FAT12_ERR_NOT_FOUND,
  FAT12_ERR_EOF,
  FAT12_ERR_FULL,
} fat12_err_t;

#define FAT12_WRITE_BATCH_MAX 36

typedef struct {
  fat12_t *fat;
  uint16_t lbas[FAT12_WRITE_BATCH_MAX];
  uint8_t data[FAT12_WRITE_BATCH_MAX][SECTOR_SIZE];
  uint8_t count;
} fat12_write_batch_t;

typedef struct {
  fat12_t *fat;
  uint16_t current_cluster;
  uint32_t file_size;
  uint32_t bytes_read;
} fat12_file_t;

typedef struct {
  fat12_t *fat;
  fat12_write_batch_t batch;
  uint16_t dirent_index;
  fat12_dirent_t dirent;
  uint16_t first_cluster;
  uint16_t current_cluster;
  uint16_t prev_cluster;
  uint32_t bytes_written;
  uint16_t cluster_offset;
  uint16_t next_free_hint;
} fat12_writer_t;

fat12_err_t fat12_init(fat12_t *fat, fat12_io_t io);
fat12_err_t fat12_format(fat12_io_t io, const char *volume_label, bool write_all_tracks);

fat12_err_t fat12_get_entry(fat12_t *fat, uint16_t cluster, uint16_t *next);
bool fat12_is_eof(uint16_t cluster);

fat12_err_t fat12_read_root_entry(fat12_t *fat, uint16_t index, fat12_dirent_t *entry);
bool fat12_entry_valid(fat12_dirent_t *entry);
bool fat12_entry_is_end(fat12_dirent_t *entry);
fat12_err_t fat12_find(fat12_t *fat, const char *filename, fat12_dirent_t *entry);

fat12_err_t fat12_open(fat12_t *fat, fat12_dirent_t *entry, fat12_file_t *file);
int fat12_read(fat12_file_t *file, uint8_t *buf, uint16_t len);
fat12_err_t fat12_read_cluster(fat12_t *fat, uint16_t cluster, uint8_t *buf);

fat12_err_t fat12_open_write(fat12_t *fat, const char *filename, fat12_writer_t *writer);
int fat12_write(fat12_writer_t *writer, const uint8_t *buf, uint16_t len);
fat12_err_t fat12_close_write(fat12_writer_t *writer);
fat12_err_t fat12_create(fat12_t *fat, const char *filename, fat12_dirent_t *entry);
fat12_err_t fat12_delete(fat12_t *fat, const char *filename);

_Static_assert(sizeof(fat12_dirent_t) == 32, "fat12_dirent_t must be 32 bytes");

#endif
