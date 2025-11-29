#ifndef FLOPPY_H
#define FLOPPY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hardware/pio.h"
#include "pico/time.h"

// ============== Configuration ==============
#define SECTOR_SIZE 512
#define FLOPPY_TRACKS 80
#define SECTORS_PER_TRACK 18

// ============== Sector/Track Structures ==============
typedef struct {
  uint8_t track;
  uint8_t side;
  uint8_t sector_n;
  uint8_t size_code;
  uint16_t size;
  uint8_t data[SECTOR_SIZE];
  bool valid;
} sector_t;

typedef struct {
  sector_t sectors[SECTORS_PER_TRACK];
  uint8_t track;
  uint8_t side;
} track_t;

// ============== Pin Configuration ==============
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

// ============== Status Codes ==============
typedef enum {
  FLOPPY_OK = 0,
  FLOPPY_ERR_WRONG_SIDE,
  FLOPPY_ERR_WRONG_TRACK,
  FLOPPY_ERR_TIMEOUT,
  FLOPPY_ERR_NO_TRACK0,
  FLOPPY_ERR_WRITE_PROTECTED,
} floppy_status_t;

// ============== PIO Program State ==============
typedef struct {
  PIO pio;
  uint sm;
  uint offset;
  uint16_t half;  // For 16-bit unpacking from 32-bit FIFO
} floppy_pio_t;

// ============== Configuration ==============
#define FLOPPY_IDLE_TIMEOUT_MS 20000  // Motor off after 20s idle

// ============== Floppy Context ==============
typedef struct floppy floppy_t;  // Forward declare for timer

struct floppy {
  floppy_pins_t pins;
  floppy_pio_t read;
  floppy_pio_t write;

  uint8_t track;
  bool track0_confirmed;
  bool disk_change_flag;  // Latched disk change state
  bool motor_on;          // Current motor state
  bool selected;          // Current select state

  // Auto motor management
  bool auto_motor;              // If true, auto select/motor on operations
  uint32_t last_io_time_ms;     // Time of last I/O
  struct repeating_timer idle_timer;
};

// ============== Lifecycle ==============

// Initialize floppy hardware (GPIO, PIO programs)
void floppy_init(floppy_t *f);

// ============== Drive Control ==============

// Select/deselect drive
void floppy_select(floppy_t *f, bool on);

// Enable/disable motor (blocks for spinup)
void floppy_motor_on(floppy_t *f);
void floppy_motor_off(floppy_t *f);

// Set density (true = HD 1.44MB, false = DD 720KB)
void floppy_set_density(floppy_t *f, bool hd);

// ============== Head Positioning ==============

// Seek to track (0-79)
floppy_status_t floppy_seek(floppy_t *f, uint8_t track);

// Get current track
uint8_t floppy_current_track(floppy_t *f);

// Check if at track 0
bool floppy_at_track0(floppy_t *f);

// ============== Disk Status ==============

// Check if disk was changed since last check
// Note: Clears the disk change flag by stepping the head
bool floppy_disk_changed(floppy_t *f);

// Check if disk is write protected
bool floppy_write_protected(floppy_t *f);

// ============== Read/Write Operations ==============

// Read a single sector
// sector->track, sector->side, sector->sector_n must be set
// On success, fills sector->data and sets sector->valid
floppy_status_t floppy_read_sector(floppy_t *f, sector_t *sector);

// Write a complete track
// Reads missing sectors (valid=false) from disk first, then writes entire track
floppy_status_t floppy_write_track(floppy_t *f, track_t *track);

// ============== f12 I/O Callbacks ==============
// These match the f12_io_t callback signatures

bool floppy_io_read(void *ctx, sector_t *sector);
bool floppy_io_write(void *ctx, track_t *track);
bool floppy_io_disk_changed(void *ctx);
bool floppy_io_write_protected(void *ctx);

#endif // FLOPPY_H
