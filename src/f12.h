#ifndef F12_H
#define F12_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "floppy.h"
#include "fat12.h"
#include "lru.h"

// ============== Configuration ==============
#define F12_MAX_OPEN_FILES 10
#define F12_CACHE_SIZE 36  // 2 tracks worth of sectors

// ============== Error Codes ==============
typedef enum {
  F12_OK = 0,
  F12_ERR_IO,              // Low-level I/O error
  F12_ERR_NOT_FOUND,       // File not found
  F12_ERR_EXISTS,          // File already exists
  F12_ERR_FULL,            // Disk full
  F12_ERR_TOO_MANY,        // Too many open files
  F12_ERR_INVALID,         // Invalid parameter
  F12_ERR_IS_DIR,          // Is a directory
  F12_ERR_NOT_MOUNTED,     // Not mounted
  F12_ERR_EOF,             // End of file
  F12_ERR_DISK_CHANGED,    // Disk was changed, must remount
  F12_ERR_WRITE_PROTECTED, // Disk is write protected
  F12_ERR_BAD_HANDLE,      // Invalid file handle
} f12_err_t;

// ============== Forward Declarations ==============
typedef struct f12 f12_t;
typedef struct f12_file f12_file_t;

// ============== File Info ==============
typedef struct {
  char name[13];  // 8.3 + null
  uint32_t size;
  uint8_t attr;
  bool is_dir;
} f12_stat_t;

// ============== Directory Iterator ==============
typedef struct {
  f12_t *fs;
  uint16_t index;
} f12_dir_t;

// ============== I/O Callbacks ==============
// These abstract the hardware layer - implement these for your platform
typedef struct {
  // Read a single sector - sector->track, sector->side, sector->sector_n must be set
  // Returns true on success, fills sector->data and sets sector->valid
  bool (*read)(void *ctx, sector_t *sector);

  // Write a complete track - fills missing sectors (valid=false) then writes whole track
  // Returns true on success
  bool (*write)(void *ctx, track_t *track);

  // Check if disk was changed (return true if changed)
  // Can be NULL if not supported
  bool (*disk_changed)(void *ctx);

  // Check if disk is write protected (return true if protected)
  // Can be NULL if not supported (assumes writable)
  bool (*write_protected)(void *ctx);

  // User context passed to callbacks
  void *ctx;
} f12_io_t;

// ============== File Handle (internal structure, exposed for static allocation) ==============
typedef enum {
  F12_MODE_CLOSED = 0,
  F12_MODE_READ,
  F12_MODE_WRITE,
} f12_file_mode_t;

struct f12_file {
  f12_t *fs;
  fat12_dirent_t dirent;
  uint16_t dirent_index;

  // For reading
  fat12_file_t reader;

  // For writing
  fat12_writer_t writer;

  f12_file_mode_t mode;
  uint32_t position;
};

// ============== Main Context ==============
struct f12 {
  f12_io_t io;
  fat12_t fat;
  lru_t *cache;

  f12_file_t files[F12_MAX_OPEN_FILES];
  f12_err_t last_error;
  bool mounted;
};

// ============== Lifecycle ==============

// Mount filesystem - initializes FAT12 and LRU cache
// Returns F12_OK on success
f12_err_t f12_mount(f12_t *fs, f12_io_t io);

// Unmount filesystem - closes all files and frees resources
void f12_unmount(f12_t *fs);

// Format disk with FAT12 filesystem
// label: volume label (up to 11 chars)
// full: if true, writes all sectors (slow but clears old data)
f12_err_t f12_format(f12_t *fs, const char *label, bool full);

// ============== File Operations ==============

// Open file for reading or writing
// path: filename (8.3 format, e.g., "FILE.TXT")
// mode: "r" = read, "w" = write (truncate/create), "a" = append
// Returns file handle on success, NULL on error (check f12_errno)
f12_file_t *f12_open(f12_t *fs, const char *path, const char *mode);

// Close file
f12_err_t f12_close(f12_file_t *file);

// Read from file
// Returns bytes read, 0 on EOF, -1 on error
int f12_read(f12_file_t *file, void *buf, size_t len);

// Write to file
// Returns bytes written, -1 on error
int f12_write(f12_file_t *file, const void *buf, size_t len);

// Seek to absolute position in file (for reading only)
f12_err_t f12_seek(f12_file_t *file, uint32_t offset);

// Get current file position
uint32_t f12_tell(f12_file_t *file);

// ============== Random Access ==============

// Read at specific offset (does not change file position for sequential read)
int f12_read_at(f12_file_t *file, uint32_t offset, void *buf, size_t len);

// Write at specific offset (only valid for files opened with "w")
int f12_write_at(f12_file_t *file, uint32_t offset, const void *buf, size_t len);

// ============== File Management ==============

// Get file info
f12_err_t f12_stat(f12_t *fs, const char *path, f12_stat_t *stat);

// Delete file
f12_err_t f12_delete(f12_t *fs, const char *path);

// ============== Directory Operations ==============

// Open directory for listing (only "/" supported for now)
f12_err_t f12_opendir(f12_t *fs, const char *path, f12_dir_t *dir);

// Read next directory entry
// Returns F12_OK and fills stat, F12_ERR_EOF when done
f12_err_t f12_readdir(f12_dir_t *dir, f12_stat_t *stat);

// Close directory
f12_err_t f12_closedir(f12_dir_t *dir);

// List all files with callback
typedef void (*f12_list_cb)(const f12_stat_t *stat, void *ctx);
f12_err_t f12_list(f12_t *fs, f12_list_cb cb, void *ctx);

// ============== Error Handling ==============

// Get last error code
f12_err_t f12_errno(f12_t *fs);

// Get error message string
const char *f12_strerror(f12_err_t err);

#endif // F12_H
