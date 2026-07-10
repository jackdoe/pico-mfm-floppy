#ifndef FAT12_H
#define FAT12_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "block.h"

#define FAT12_DIR_ENTRY_SIZE 32u
#define FAT12_FILENAME_LEN 8u
#define FAT12_EXTENSION_LEN 3u
#define FAT12_MAX_CLUSTER_SECTORS 1u
#define FAT12_RESERVED_SECTORS 1u
#define FAT12_NUM_FATS 2u
#define FAT12_ROOT_ENTRIES 224u
#define FAT12_SECTORS_PER_FAT 9u
#define FAT12_MEDIA_DESCRIPTOR 0xF0u
#define FAT12_ROOT_DIR_SECTORS \
  ((FAT12_ROOT_ENTRIES * FAT12_DIR_ENTRY_SIZE + DISK_SECTOR_SIZE - 1u) / DISK_SECTOR_SIZE)
#define FAT12_DATA_START \
  (FAT12_RESERVED_SECTORS + FAT12_NUM_FATS * FAT12_SECTORS_PER_FAT + FAT12_ROOT_DIR_SECTORS)
#define FAT12_DATA_CLUSTERS (DISK_SECTOR_COUNT - FAT12_DATA_START)
#define FAT12_CLUSTER_LIMIT (FAT12_DATA_CLUSTERS + 2u)
#define FAT12_CLUSTER_BITMAP_BYTES ((FAT12_CLUSTER_LIMIT + 7u) / 8u)
#define FAT12_FAT1_START FAT12_RESERVED_SECTORS
#define FAT12_FAT2_START (FAT12_RESERVED_SECTORS + FAT12_SECTORS_PER_FAT)
#define FAT12_BPB_OFFSET 11u
#define FAT12_BOOT_SIG_OFFSET 510u

typedef void (*fat12_progress_t)(void *ctx, uint8_t cylinder, uint8_t head,
                                 uint16_t done, uint16_t total);

typedef struct {
  block_status_t (*read)(void *ctx, uint16_t lba,
                         uint8_t out[DISK_SECTOR_SIZE]);
  block_status_t (*write)(void *ctx, const track_t *partial_track);
  void *ctx;
} fat12_io_t;

typedef struct {
  char name[FAT12_FILENAME_LEN];
  char ext[FAT12_EXTENSION_LEN];
} fat12_name_t;

typedef struct {
  char name[FAT12_FILENAME_LEN];
  char ext[FAT12_EXTENSION_LEN];
  uint8_t attr;
  uint8_t reserved[10];
  uint16_t time;
  uint16_t date;
  uint16_t start_cluster;
  uint32_t size;
} fat12_dirent_t;

#define FAT12_ATTR_READ_ONLY 0x01u
#define FAT12_ATTR_HIDDEN 0x02u
#define FAT12_ATTR_SYSTEM 0x04u
#define FAT12_ATTR_VOLUME_ID 0x08u
#define FAT12_ATTR_DIRECTORY 0x10u
#define FAT12_ATTR_ARCHIVE 0x20u
#define FAT12_ATTR_LFN 0x0Fu

#define FAT12_DIRENT_FREE 0xE5u
#define FAT12_DIRENT_END 0x00u
#define FAT12_WRITE_BATCH_MAX (FAT12_NUM_FATS * FAT12_SECTORS_PER_FAT)

_Static_assert(FAT12_WRITE_BATCH_MAX <= UINT8_MAX, "batch count must fit uint8_t");
_Static_assert(FAT12_ROOT_DIR_SECTORS <= FAT12_WRITE_BATCH_MAX,
               "root metadata must fit batch");

typedef struct {
  uint16_t lbas[FAT12_WRITE_BATCH_MAX];
  uint8_t data[FAT12_WRITE_BATCH_MAX][DISK_SECTOR_SIZE];
  uint8_t count;
  bool active;
} fat12_write_batch_t;

typedef struct fat12 {
  fat12_io_t io;
  block_status_t last_io;
  uint16_t fat_start;
  bool fat_mismatch;
  bool fat_markers_invalid;
  fat12_write_batch_t batch;
  fat12_write_batch_t namespace_batch;
  track_t write_track;
  uint16_t fsck_owner[FAT12_CLUSTER_LIMIT];
  uint16_t fsck_directories[FAT12_DATA_CLUSTERS];
  uint8_t fsck_reachable[FAT12_CLUSTER_BITMAP_BYTES];
} fat12_t;

typedef enum {
  FAT12_OK = 0,
  FAT12_ERR_READ,
  FAT12_ERR_WRITE,
  FAT12_ERR_INVALID,
  FAT12_ERR_NOT_FOUND,
  FAT12_ERR_EOF,
  FAT12_ERR_FULL,
  FAT12_ERR_EXISTS,
  FAT12_ERR_READ_ONLY,
  FAT12_ERR_BUSY,
  FAT12_ERR_CORRUPT,
  FAT12_ERR_AMBIGUOUS,
} fat12_err_t;

typedef struct {
  fat12_err_t error;
  size_t count;
} fat12_result_t;

typedef struct {
  fat12_t *fat;
  uint16_t start_cluster;
  uint16_t current_cluster;
  uint32_t file_size;
  uint32_t bytes_read;
  bool buffer_valid;
  uint8_t cluster_buf[FAT12_MAX_CLUSTER_SECTORS * DISK_SECTOR_SIZE];
} fat12_file_t;

typedef enum {
  FAT12_WRITER_DATA = 0,
  FAT12_WRITER_FLUSH_NEW,
  FAT12_WRITER_STAGE_DIRENT,
  FAT12_WRITER_FLUSH_DIRENT,
  FAT12_WRITER_PREPARE_RECLAIM,
  FAT12_WRITER_FLUSH_RECLAIM,
  FAT12_WRITER_DONE,
} fat12_writer_phase_t;

typedef struct {
  fat12_t *fat;
  uint16_t dirent_index;
  fat12_dirent_t dirent;
  uint16_t first_cluster;
  uint16_t current_cluster;
  uint16_t prev_cluster;
  uint16_t pending_cluster;
  uint32_t bytes_written;
  fat12_err_t error;
  fat12_writer_phase_t phase;
  bool replacing_existing;
  bool consumed_end;
  uint8_t cluster_buf[FAT12_MAX_CLUSTER_SECTORS * DISK_SECTOR_SIZE];
} fat12_writer_t;

typedef struct {
  uint16_t files;
  uint16_t directories;
  uint16_t lost_clusters;
  uint16_t crosslinked;
  uint16_t loops;
  uint16_t broken_chains;
  uint16_t size_mismatches;
  uint16_t truncated_files;
  uint16_t removed_directories;
  uint16_t duplicate_names;
  uint16_t removed_duplicates;
  uint16_t freed_tails;
  uint16_t freed;
  uint32_t fat1_score;
  uint32_t fat2_score;
  uint8_t authoritative_fat;
  bool fat_mismatch;
  bool fat_markers_invalid;
  bool fat_ambiguous;
  bool repaired_fat1;
  bool repaired_fat2;
  bool repair_pending;
  bool incomplete;
} fat12_fsck_t;

fat12_err_t fat12_init(fat12_t *fat, fat12_io_t io);
fat12_err_t fat12_format(fat12_t *fat, fat12_io_t io, const char *volume_label,
                         bool write_all_tracks, fat12_progress_t progress,
                         void *progress_ctx);
fat12_err_t fat12_fsck(fat12_t *fat, fat12_fsck_t *out, bool repair);
fat12_err_t fat12_abort_write(fat12_writer_t *writer);
void fat12_forget_write(fat12_writer_t *writer);
bool fat12_busy(const fat12_t *fat);
block_status_t fat12_last_io(const fat12_t *fat);

fat12_err_t fat12_get_entry(fat12_t *fat, uint16_t cluster, uint16_t *next);
fat12_err_t fat12_free_count(fat12_t *fat, uint16_t *count);
bool fat12_is_eof(uint16_t cluster);

fat12_err_t fat12_name_parse(const char *input, fat12_name_t *out);
fat12_err_t fat12_read_root_entry(fat12_t *fat, uint16_t index,
                                  fat12_dirent_t *entry);
bool fat12_entry_valid(const fat12_dirent_t *entry);
bool fat12_entry_is_end(const fat12_dirent_t *entry);
fat12_err_t fat12_find(fat12_t *fat, const char *filename,
                       fat12_dirent_t *entry);

fat12_err_t fat12_open(fat12_t *fat, const fat12_dirent_t *entry,
                       fat12_file_t *file);
fat12_err_t fat12_seek(fat12_file_t *file, uint32_t offset);
fat12_result_t fat12_read(fat12_file_t *file, uint8_t *buf, size_t len);
fat12_err_t fat12_read_cluster(fat12_t *fat, uint16_t cluster, uint8_t *buf);

fat12_err_t fat12_open_write(fat12_t *fat, const char *filename,
                             fat12_writer_t *writer);
fat12_result_t fat12_write(fat12_writer_t *writer, const uint8_t *buf,
                           size_t len);
fat12_err_t fat12_close_write(fat12_writer_t *writer);
fat12_err_t fat12_delete(fat12_t *fat, const char *filename);
fat12_err_t fat12_rename(fat12_t *fat, const char *from, const char *to);

_Static_assert(sizeof(fat12_dirent_t) == FAT12_DIR_ENTRY_SIZE,
               "fat12_dirent_t size");

#endif
