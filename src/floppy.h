#ifndef FLOPPY_H
#define FLOPPY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hardware/pio.h"
#include "pico/time.h"

#define SECTOR_SIZE 512
#define FLOPPY_TRACKS 80
#define SECTORS_PER_TRACK 18

typedef struct {
  uint8_t track;
  uint8_t side;
  uint8_t sector_n;
  uint8_t size_code;
  uint8_t data[SECTOR_SIZE];
  bool valid;
} sector_t;

static inline uint16_t sector_size(const sector_t *s) {
  return 128 << s->size_code;
}

typedef struct {
  sector_t sectors[SECTORS_PER_TRACK];
  uint8_t track;
  uint8_t side;
} track_t;

typedef struct {
  uint8_t index;
  uint8_t track0;
  uint8_t write_protect;
  uint8_t read_data;
  uint8_t disk_change;
  uint8_t drive_select;
  uint8_t motor_enable;
  uint8_t direction;
  uint8_t step;
  uint8_t write_data;
  uint8_t write_gate;
  uint8_t side_select;
  uint8_t density;
} floppy_pins_t;

typedef enum {
  FLOPPY_OK = 0,
  FLOPPY_ERR_WRONG_SIDE,
  FLOPPY_ERR_WRONG_TRACK,
  FLOPPY_ERR_TIMEOUT,
  FLOPPY_ERR_NO_TRACK0,
  FLOPPY_ERR_WRITE_PROTECTED,
} floppy_status_t;

typedef struct {
  PIO pio;
  uint sm;
  uint offset;
  uint16_t half;
} floppy_pio_t;

#define FLOPPY_IDLE_TIMEOUT_MS 20000

typedef struct floppy floppy_t;

struct floppy {
  floppy_pins_t pins;
  floppy_pio_t read;
  floppy_pio_t write;

  uint8_t track;
  bool track0_confirmed;
  bool disk_change_flag;
  volatile bool motor_on;
  volatile bool selected;

  bool auto_motor;
  volatile uint32_t last_io_time_ms;
  struct repeating_timer idle_timer;
};

void floppy_init(floppy_t *f);

void floppy_select(floppy_t *f, bool on);

void floppy_motor_on(floppy_t *f);
void floppy_motor_off(floppy_t *f);

void floppy_set_density(floppy_t *f, bool hd);

floppy_status_t floppy_seek(floppy_t *f, uint8_t track);

uint8_t floppy_current_track(floppy_t *f);

bool floppy_at_track0(floppy_t *f);

bool floppy_disk_changed(floppy_t *f);

bool floppy_write_protected(floppy_t *f);

floppy_status_t floppy_read_sector(floppy_t *f, sector_t *sector);
floppy_status_t floppy_read_track(floppy_t *f, track_t *t);

floppy_status_t floppy_write_track(floppy_t *f, track_t *track);

bool floppy_io_read(void *ctx, sector_t *sector);
bool floppy_io_read_track(void *ctx, track_t *track);
bool floppy_io_write(void *ctx, track_t *track);
bool floppy_io_disk_changed(void *ctx);
bool floppy_io_write_protected(void *ctx);

#endif
