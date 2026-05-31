#ifndef FAT12_H
#define FAT12_H

#include <stdint.h>
#include <stdbool.h>
#include "floppy.h"

#define FAT12_DIR_ENTRY_SIZE 32
#define FAT12_FILENAME_LEN 8
#define FAT12_EXTENSION_LEN 3
#define FAT12_MAX_CLUSTER_SECTORS 1

typedef void (*fat12_progress_t)(void *ctx, uint8_t cyl, uint8_t side,
                                 uint16_t done, uint16_t total);

typedef struct {
  bool (*read)(void *ctx, sector_t *sector);
  bool (*write)(void *ctx, track_t *track);
  fat12_progress_t progress;
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
} __attribute__((packed)) fat12_bpb_t;

_Static_assert(sizeof(fat12_bpb_t) == 21, "fat12_bpb_t must match on-disk BPB layout");

#define FAT12_BPB_OFFSET 11
#define FAT12_BOOT_SIG_OFFSET 510

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

#define FAT12_DIRENT_FREE     0xE5
#define FAT12_DIRENT_END      0x00

#define FAT12_WRITE_BATCH_MAX 36

typedef struct {
  uint16_t lbas[FAT12_WRITE_BATCH_MAX];
  uint8_t data[FAT12_WRITE_BATCH_MAX][SECTOR_SIZE];
  uint8_t count;
  bool active;
} fat12_write_batch_t;

typedef struct fat12 {
  fat12_io_t io;
  fat12_bpb_t bpb;

  uint16_t fat_start_sector;
  uint16_t root_dir_start_sector;
  uint16_t root_dir_sectors;
  uint16_t data_start_sector;
  uint16_t total_clusters;

  fat12_write_batch_t batch;

  uint16_t next_free_hint;
  bool fat_mismatch;
  track_t write_track;
} fat12_t;

static inline uint16_t fat12_chs_to_lba(const fat12_bpb_t *bpb,
                                         uint8_t track, uint8_t side, uint8_t sector_n) {
  return (track * bpb->num_heads + side) * bpb->sectors_per_track + (sector_n - 1);
}

static inline void fat12_lba_to_chs(const fat12_bpb_t *bpb, uint16_t lba,
                                     uint8_t *track, uint8_t *side, uint8_t *sector_n) {
  uint16_t per_cyl = bpb->num_heads * bpb->sectors_per_track;
  *track = lba / per_cyl;
  uint16_t rem = lba % per_cyl;
  *side = rem / bpb->sectors_per_track;
  *sector_n = (rem % bpb->sectors_per_track) + 1;
}

typedef enum {
  FAT12_OK = 0,
  FAT12_ERR_READ,
  FAT12_ERR_WRITE,
  FAT12_ERR_INVALID,
  FAT12_ERR_NOT_FOUND,
  FAT12_ERR_EOF,
  FAT12_ERR_FULL,
} fat12_err_t;

typedef struct {
  fat12_t *fat;
  uint16_t start_cluster;
  uint16_t current_cluster;
  uint16_t buffer_cluster;
  uint32_t file_size;
  uint32_t bytes_read;
  bool buffer_valid;
  uint8_t cluster_buf[FAT12_MAX_CLUSTER_SECTORS * SECTOR_SIZE];
} fat12_file_t;

typedef struct {
  fat12_t *fat;
  uint16_t dirent_index;
  fat12_dirent_t dirent;
  uint16_t first_cluster;
  uint16_t current_cluster;
  uint16_t old_start_cluster;
  uint16_t prev_cluster;
  uint32_t bytes_written;
  uint16_t cluster_offset;
  fat12_err_t error;
  bool replacing_existing;
  bool overwrite_in_place;
  bool cluster_loaded;
  bool cluster_dirty;
  uint8_t cluster_buf[FAT12_MAX_CLUSTER_SECTORS * SECTOR_SIZE];
} fat12_writer_t;

fat12_err_t fat12_init(fat12_t *fat, fat12_io_t io);
fat12_err_t fat12_format(fat12_io_t io, const char *volume_label, bool write_all_tracks);
void fat12_abort_write(fat12_t *fat);

fat12_err_t fat12_get_entry(fat12_t *fat, uint16_t cluster, uint16_t *next);
bool fat12_is_eof(uint16_t cluster);

fat12_err_t fat12_read_root_entry(fat12_t *fat, uint16_t index, fat12_dirent_t *entry);
bool fat12_entry_valid(fat12_dirent_t *entry);
bool fat12_entry_is_end(fat12_dirent_t *entry);
fat12_err_t fat12_find(fat12_t *fat, const char *filename, fat12_dirent_t *entry);

void fat12_open(fat12_t *fat, fat12_dirent_t *entry, fat12_file_t *file);
fat12_err_t fat12_seek(fat12_file_t *file, uint32_t offset);
int fat12_read(fat12_file_t *file, uint8_t *buf, size_t len);
fat12_err_t fat12_read_cluster(fat12_t *fat, uint16_t cluster, uint8_t *buf);

fat12_err_t fat12_open_write(fat12_t *fat, const char *filename, fat12_writer_t *writer);
int fat12_write(fat12_writer_t *writer, const uint8_t *buf, size_t len);
fat12_err_t fat12_close_write(fat12_writer_t *writer);
fat12_err_t fat12_create(fat12_t *fat, const char *filename, fat12_dirent_t *entry);
fat12_err_t fat12_delete(fat12_t *fat, const char *filename);

_Static_assert(sizeof(fat12_dirent_t) == 32, "fat12_dirent_t must be 32 bytes");

#endif
