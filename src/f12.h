#ifndef F12_H
#define F12_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "block.h"
#include "fat12.h"

#define F12_MAX_OPEN_FILES 4u
#define F12_MAX_OPEN_DIRS 4u
#define F12_CACHE_TRACKS 3u

typedef enum {
  F12_OK = 0,
  F12_END,
  F12_ERR_INVALID,
  F12_ERR_NOT_INITIALIZED,
  F12_ERR_NOT_MOUNTED,
  F12_ERR_ALREADY_MOUNTED,
  F12_ERR_NOT_FOUND,
  F12_ERR_EXISTS,
  F12_ERR_FULL,
  F12_ERR_TOO_MANY,
  F12_ERR_IS_DIR,
  F12_ERR_NOT_DIR,
  F12_ERR_READ_ONLY,
  F12_ERR_CONFLICT,
  F12_ERR_BUSY,
  F12_ERR_BAD_HANDLE,
  F12_ERR_TIMEOUT,
  F12_ERR_CRC,
  F12_ERR_WRONG_TRACK,
  F12_ERR_WRONG_SIDE,
  F12_ERR_NO_TRACK0,
  F12_ERR_MEDIA_CHANGED,
  F12_ERR_WRITE_PROTECTED,
  F12_ERR_UNDERRUN,
  F12_ERR_OVERRUN,
  F12_ERR_VERIFY,
  F12_ERR_AMBIGUOUS,
  F12_ERR_CORRUPT,
  F12_ERR_IO,
} f12_err_t;

typedef struct f12 f12_t;

typedef struct {
  f12_t *fs;
  uint64_t incarnation;
  uint64_t mount_generation;
  uint64_t slot_generation;
  uint16_t slot;
} f12_token_t;

typedef enum {
  F12_OPEN_READ = 1,
  F12_OPEN_WRITE,
} f12_open_mode_t;

typedef struct {
  f12_token_t token;
} f12_file_t;

typedef struct {
  f12_token_t token;
} f12_dir_t;

typedef struct {
  f12_err_t error;
  size_t count;
} f12_result_t;

typedef enum {
  F12_FORMAT_QUICK = 0,
  F12_FORMAT_FULL,
} f12_format_mode_t;

typedef void (*f12_progress_t)(void *ctx, uint8_t cylinder, uint8_t head,
                               uint16_t done, uint16_t total);

typedef struct {
  const char *label;
  f12_format_mode_t mode;
  f12_progress_t progress;
  void *progress_ctx;
} f12_format_options_t;

typedef struct {
  char name[13];
  uint32_t size;
  uint8_t attr;
} f12_stat_t;

typedef struct {
  track_t track;
  uint64_t used;
  uint32_t conflicted;
  bool occupied;
} f12_cache_entry_t;

typedef enum {
  F12_STATE_READY = 0,
  F12_STATE_BLOCK_ACCESS,
  F12_STATE_MOUNTED,
} f12_state_t;

typedef struct {
  fat12_name_t name;
  union {
    fat12_file_t reader;
    fat12_writer_t writer;
  } io;
  uint64_t generation;
  f12_open_mode_t mode;
  bool active;
} f12_file_slot_t;

typedef struct {
  uint64_t generation;
  uint16_t index;
  bool active;
} f12_dir_slot_t;

struct f12 {
  block_device_t device;
  fat12_t fat;
  f12_cache_entry_t cache[F12_CACHE_TRACKS];
  track_t track_work;
  f12_file_slot_t files[F12_MAX_OPEN_FILES];
  f12_dir_slot_t dirs[F12_MAX_OPEN_DIRS];
  uint64_t cache_clock;
  uint64_t incarnation;
  uint32_t media_generation;
  uint64_t mount_generation;
  uint32_t signature;
  uint32_t signature_inverse;
  f12_state_t state;
};

f12_err_t f12_init(f12_t *fs, block_device_t device);
f12_err_t f12_mount(f12_t *fs);
f12_err_t f12_unmount(f12_t *fs);
f12_err_t f12_format(f12_t *fs, f12_format_options_t options);
f12_err_t f12_fsck(f12_t *fs, fat12_fsck_t *report, bool repair);
f12_err_t f12_is_mounted(f12_t *fs, bool *mounted);

f12_err_t f12_open(f12_t *fs, const char *path, f12_open_mode_t mode,
                   f12_file_t *out);
f12_err_t f12_close(f12_file_t *file);
f12_err_t f12_abort(f12_file_t *file);
f12_result_t f12_read(f12_file_t *file, void *buf, size_t len);
f12_result_t f12_write(f12_file_t *file, const void *buf, size_t len);
f12_err_t f12_seek(f12_file_t *file, uint32_t offset);
f12_err_t f12_tell(const f12_file_t *file, uint32_t *offset);
f12_result_t f12_read_at(f12_file_t *file, uint32_t offset, void *buf,
                         size_t len);

f12_err_t f12_stat(f12_t *fs, const char *path, f12_stat_t *stat);
f12_err_t f12_free_count(f12_t *fs, uint16_t *count);
f12_err_t f12_delete(f12_t *fs, const char *path);
f12_err_t f12_rename(f12_t *fs, const char *from, const char *to);

f12_err_t f12_opendir(f12_t *fs, const char *path, f12_dir_t *dir);
f12_err_t f12_readdir(f12_dir_t *dir, f12_stat_t *stat);
f12_err_t f12_closedir(f12_dir_t *dir);

typedef f12_err_t (*f12_list_fn)(void *ctx, const f12_stat_t *stat);
f12_err_t f12_list(f12_t *fs, f12_list_fn fn, void *ctx);

const char *f12_strerror(f12_err_t err);

#endif
