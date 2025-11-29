#ifndef FAT12_H
#define FAT12_H

#include <stdint.h>
#include <stdbool.h>
#include "floppy.h"

// ============== Configuration ==============
#define FAT12_DIR_ENTRY_SIZE 32
#define FAT12_FILENAME_LEN 8
#define FAT12_EXTENSION_LEN 3

// ============== Disk I/O Callbacks ==============
typedef struct {
  // Read a single sector - sector->track, sector->side, sector->sector_n must be set
  // Returns true on success, fills sector->data and sets sector->valid
  bool (*read)(void *ctx, sector_t *sector);

  // Write a complete track - fills missing sectors (valid=false) then writes whole track
  // Set valid=true for sectors you want to write, valid=false for sectors to read from disk
  // Returns true on success
  bool (*write)(void *ctx, track_t *track);

  void *ctx;
} fat12_io_t;

// ============== Boot Sector / BPB ==============
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

// ============== Directory Entry ==============
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

// File attributes
#define FAT12_ATTR_READ_ONLY  0x01
#define FAT12_ATTR_HIDDEN     0x02
#define FAT12_ATTR_SYSTEM     0x04
#define FAT12_ATTR_VOLUME_ID  0x08
#define FAT12_ATTR_DIRECTORY  0x10
#define FAT12_ATTR_ARCHIVE    0x20
#define FAT12_ATTR_LFN        0x0F

// ============== FAT12 Context ==============
typedef struct {
  fat12_io_t io;
  fat12_bpb_t bpb;

  // Computed values
  uint16_t fat_start_sector;      // First sector of FAT
  uint16_t root_dir_start_sector; // First sector of root directory
  uint16_t root_dir_sectors;      // Number of sectors in root dir
  uint16_t data_start_sector;     // First sector of data area
  uint16_t total_clusters;        // Total data clusters

  // Sector buffer for internal use
  sector_t sector_buf;
} fat12_t;

// ============== Error codes ==============
typedef enum {
  FAT12_OK = 0,
  FAT12_ERR_READ,
  FAT12_ERR_WRITE,
  FAT12_ERR_INVALID,
  FAT12_ERR_NOT_FOUND,
  FAT12_ERR_EOF,
  FAT12_ERR_FULL,
} fat12_err_t;

// ============== Write batch ==============
#define FAT12_WRITE_BATCH_MAX 36  // 2 tracks worth of sectors

typedef struct {
  fat12_t *fat;
  uint16_t lbas[FAT12_WRITE_BATCH_MAX];
  uint8_t data[FAT12_WRITE_BATCH_MAX][SECTOR_SIZE];
  uint8_t count;
} fat12_write_batch_t;

// ============== File reader context ==============
typedef struct {
  fat12_t *fat;
  uint16_t current_cluster;
  uint32_t file_size;
  uint32_t bytes_read;
} fat12_file_t;

// ============== File writer context ==============
typedef struct {
  fat12_t *fat;
  fat12_write_batch_t batch;
  uint16_t dirent_index;
  fat12_dirent_t dirent;
  uint16_t first_cluster;
  uint16_t current_cluster;
  uint16_t prev_cluster;
  uint32_t bytes_written;
  uint16_t cluster_offset;  // bytes into current cluster
  uint16_t next_free_hint;  // hint for next free cluster search
} fat12_writer_t;

// ============== Core API ==============

// Initialize FAT12 from boot sector
fat12_err_t fat12_init(fat12_t *fat, fat12_io_t io);

// Format disk with FAT12 filesystem
fat12_err_t fat12_format(fat12_io_t io, const char *volume_label, bool write_all_tracks);

// ============== FAT Operations ==============

// Get next cluster in chain (returns cluster value in *next)
fat12_err_t fat12_get_entry(fat12_t *fat, uint16_t cluster, uint16_t *next);

// Check cluster status
bool fat12_is_eof(uint16_t cluster);
bool fat12_is_free(uint16_t cluster);
bool fat12_is_bad(uint16_t cluster);

// ============== Directory Operations ==============

// Read root directory entry by index
fat12_err_t fat12_read_root_entry(fat12_t *fat, uint16_t index, fat12_dirent_t *entry);

// Check entry validity
bool fat12_entry_valid(fat12_dirent_t *entry);
bool fat12_entry_is_end(fat12_dirent_t *entry);

// Find file in root directory by name (accepts "FILE.TXT" format)
fat12_err_t fat12_find(fat12_t *fat, const char *filename, fat12_dirent_t *entry);

// Format filename for FAT12 (8.3 format)
void fat12_format_name(const char *input, char *name8, char *ext3);

// ============== File Read Operations ==============

// Open file for reading
fat12_err_t fat12_open(fat12_t *fat, fat12_dirent_t *entry, fat12_file_t *file);

// Read from file (returns bytes read, or negative error)
int fat12_read(fat12_file_t *file, uint8_t *buf, uint16_t len);

// Read single cluster
fat12_err_t fat12_read_cluster(fat12_t *fat, uint16_t cluster, uint8_t *buf);

// ============== File Write Operations ==============

// Open file for writing (creates or truncates)
fat12_err_t fat12_open_write(fat12_t *fat, const char *filename, fat12_writer_t *writer);

// Write to file (returns bytes written, or negative error)
int fat12_write(fat12_writer_t *writer, const uint8_t *buf, uint16_t len);

// Close file and flush writes
fat12_err_t fat12_close_write(fat12_writer_t *writer);

// Create new file
fat12_err_t fat12_create(fat12_t *fat, const char *filename, fat12_dirent_t *entry);

// Delete file
fat12_err_t fat12_delete(fat12_t *fat, const char *filename);

// ============== Write Batch Operations ==============

void fat12_write_batch_init(fat12_write_batch_t *batch, fat12_t *fat);
fat12_err_t fat12_write_batch_add(fat12_write_batch_t *batch, uint16_t lba, const uint8_t *data);
fat12_err_t fat12_write_batch_flush(fat12_write_batch_t *batch);

// ============== Internal helpers (exposed for testing) ==============

void fat12_lba_to_chs(fat12_t *fat, uint16_t lba, uint8_t *c, uint8_t *h, uint8_t *s);
bool fat12_read_sector(fat12_t *fat, uint16_t lba, sector_t *sector);
uint16_t fat12_cluster_to_lba(fat12_t *fat, uint16_t cluster);

#endif // FAT12_H
