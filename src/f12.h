#ifndef F12_H
#define F12_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "floppy.h"
#include "fat12.h"
#include "lru.h"

#define F12_MAX_OPEN_FILES 10
#define F12_CACHE_SIZE 36

typedef enum {
  F12_OK = 0,
  F12_ERR_IO,
  F12_ERR_NOT_FOUND,
  F12_ERR_EXISTS,
  F12_ERR_FULL,
  F12_ERR_TOO_MANY,
  F12_ERR_INVALID,
  F12_ERR_IS_DIR,
  F12_ERR_NOT_MOUNTED,
  F12_ERR_EOF,
  F12_ERR_DISK_CHANGED,
  F12_ERR_WRITE_PROTECTED,
  F12_ERR_BAD_HANDLE,
} f12_err_t;

typedef struct f12 f12_t;
typedef struct f12_file f12_file_t;

typedef struct {
  char name[13];
  uint32_t size;
  uint8_t attr;
  bool is_dir;
} f12_stat_t;

typedef struct {
  f12_t *fs;
  uint16_t index;
} f12_dir_t;

typedef struct {
  bool (*read)(void *ctx, sector_t *sector);
  bool (*write)(void *ctx, track_t *track);
  bool (*disk_changed)(void *ctx);
  bool (*write_protected)(void *ctx);
  void *ctx;
} f12_io_t;

typedef enum {
  F12_MODE_CLOSED = 0,
  F12_MODE_READ,
  F12_MODE_WRITE,
} f12_file_mode_t;

struct f12_file {
  f12_t *fs;
  fat12_dirent_t dirent;
  uint16_t dirent_index;
  fat12_file_t reader;
  fat12_writer_t writer;
  f12_file_mode_t mode;
  uint32_t position;
};

struct f12 {
  f12_io_t io;
  fat12_t fat;
  lru_t *cache;

  f12_file_t files[F12_MAX_OPEN_FILES];
  f12_err_t last_error;
  bool mounted;
};

f12_err_t f12_mount(f12_t *fs, f12_io_t io);
void f12_unmount(f12_t *fs);
f12_err_t f12_format(f12_t *fs, const char *label, bool full);

f12_file_t *f12_open(f12_t *fs, const char *path, const char *mode);
f12_err_t f12_close(f12_file_t *file);
int f12_read(f12_file_t *file, void *buf, size_t len);
int f12_write(f12_file_t *file, const void *buf, size_t len);
f12_err_t f12_seek(f12_file_t *file, uint32_t offset);
uint32_t f12_tell(f12_file_t *file);

int f12_read_at(f12_file_t *file, uint32_t offset, void *buf, size_t len);

f12_err_t f12_stat(f12_t *fs, const char *path, f12_stat_t *stat);
f12_err_t f12_delete(f12_t *fs, const char *path);

f12_err_t f12_opendir(f12_t *fs, const char *path, f12_dir_t *dir);
f12_err_t f12_readdir(f12_dir_t *dir, f12_stat_t *stat);
f12_err_t f12_closedir(f12_dir_t *dir);

typedef void (*f12_list_cb)(const f12_stat_t *stat, void *ctx);
f12_err_t f12_list(f12_t *fs, f12_list_cb cb, void *ctx);

f12_err_t f12_errno(f12_t *fs);
const char *f12_strerror(f12_err_t err);

#endif
