#ifndef F12_H
#define F12_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cache.h"
#include "fat12.h"

#define F12_MAX_OPEN_FILES 4u
#define F12_MAX_OPEN_DIRS 4u

typedef struct f12 f12_t;

typedef struct {
  f12_t *fs;
  uint64_t incarnation;
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

typedef enum {
  F12_FORMAT_QUICK = 0,
  F12_FORMAT_FULL,
} f12_format_mode_t;

typedef struct {
  const char *label;
  f12_format_mode_t mode;
  fat12_progress_t progress;
  void *progress_ctx;
} f12_format_options_t;

typedef struct {
  char name[13];
  uint32_t size;
  uint8_t attr;
} f12_stat_t;

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
  cache_t cache;
  fat12_t fat;
  f12_file_slot_t files[F12_MAX_OPEN_FILES];
  f12_dir_slot_t dirs[F12_MAX_OPEN_DIRS];
  uint64_t incarnation;
  uint32_t signature;
  uint32_t signature_inverse;
  f12_state_t state;
};

disk_err_t f12_init(f12_t *fs, disk_device_t device);
disk_err_t f12_mount(f12_t *fs);
disk_err_t f12_unmount(f12_t *fs);
disk_err_t f12_format(f12_t *fs, f12_format_options_t options);
disk_err_t f12_fsck(f12_t *fs, fat12_fsck_t *report, bool repair);
disk_err_t f12_is_mounted(f12_t *fs, bool *mounted);

disk_err_t f12_open(f12_t *fs, const char *path, f12_open_mode_t mode,
                    f12_file_t *out);
disk_err_t f12_close(f12_file_t *file);
disk_err_t f12_abort(f12_file_t *file);
disk_result_t f12_read(f12_file_t *file, void *buf, size_t len);
disk_result_t f12_write(f12_file_t *file, const void *buf, size_t len);
disk_err_t f12_seek(f12_file_t *file, uint32_t offset);
disk_err_t f12_tell(const f12_file_t *file, uint32_t *offset);
disk_result_t f12_read_at(f12_file_t *file, uint32_t offset, void *buf,
                          size_t len);

disk_err_t f12_stat(f12_t *fs, const char *path, f12_stat_t *stat);
disk_err_t f12_free_count(f12_t *fs, uint16_t *count);
disk_err_t f12_delete(f12_t *fs, const char *path);
disk_err_t f12_rename(f12_t *fs, const char *from, const char *to);

disk_err_t f12_opendir(f12_t *fs, const char *path, f12_dir_t *dir);
disk_err_t f12_readdir(f12_dir_t *dir, f12_stat_t *stat);
disk_err_t f12_closedir(f12_dir_t *dir);

typedef disk_err_t (*f12_list_fn)(void *ctx, const f12_stat_t *stat);
disk_err_t f12_list(f12_t *fs, f12_list_fn fn, void *ctx);

#endif
