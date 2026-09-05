#ifndef FLOPPY_H
#define FLOPPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "disk.h"
#include "hardware/pio.h"

#define FLOPPY_FLUX_RING_BITS 12u
#define FLOPPY_FLUX_RING_BYTES (1u << FLOPPY_FLUX_RING_BITS)
#define FLOPPY_FLUX_RING_WORDS (FLOPPY_FLUX_RING_BYTES / 4u)
#define FLOPPY_WRITE_PIO_OVERHEAD 19u
#define FLOPPY_TX_HALF_BYTES (FLOPPY_FLUX_RING_BYTES / 2u)
#define FLOPPY_IDLE_TIMEOUT_MS 20000u

typedef enum {
  FLOPPY_OPERATION_IDLE = 0,
  FLOPPY_OPERATION_CONTROL,
  FLOPPY_OPERATION_RAW,
  FLOPPY_OPERATION_READ,
  FLOPPY_OPERATION_WRITE,
  FLOPPY_OPERATION_TEARDOWN,
} floppy_operation_t;

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

typedef struct {
  PIO pio;
  uint sm;
  uint offset;
  uint16_t half;
  bool half_valid;
  uint16_t prev;
  bool primed;
} floppy_pio_t;

typedef struct {
  uint16_t delta;
  bool index;
} floppy_pulse_t;

typedef struct {
  uint32_t reads;
  uint32_t retries;
  uint32_t recovered;
  uint32_t failed;
  uint32_t timeout;
  uint32_t crc;
  uint32_t wrong_track;
  uint32_t wrong_side;
  uint32_t overruns;
  uint32_t underruns;
  uint32_t media_changes;
  uint32_t flux_words;
  uint32_t ring_peak;
  uint32_t dma_writes;
} floppy_stats_t;

typedef struct floppy {
  uint32_t flux_ring[FLOPPY_FLUX_RING_WORDS]
      __attribute__((aligned(1u << FLOPPY_FLUX_RING_BITS)));
  floppy_pins_t pins;
  floppy_pio_t read;
  floppy_pio_t write;
  int dma_ch;
  uint32_t ring_cpu;
  uint8_t cylinder;
  bool track0_confirmed;
  bool disk_change_active;
  bool media_observed;
  bool read_overrun;
  bool motor_on;
  bool selected;
  bool motor_qualified;
  bool head_confirmed;
  uint8_t head;
  bool read_program_loaded;
  bool write_program_loaded;
  bool read_sm_claimed;
  bool write_sm_claimed;
  bool dma_claimed;
  bool gpio_configured;
  bool flux_active;
  uint32_t last_io_time_ms;
  uint32_t media_generation;
  uint32_t motor_generation;
  uint32_t flux_generation;
  floppy_operation_t operation;
  uint32_t operation_start_ms;
  uint32_t operation_limit_ms;
  uint32_t lifecycle;
  floppy_stats_t stats;
} floppy_t;

disk_err_t floppy_init(floppy_t *f, floppy_pins_t pins);
disk_err_t floppy_deinit(floppy_t *f);
disk_err_t floppy_poll(floppy_t *f);
disk_err_t floppy_select(floppy_t *f, bool on);
disk_err_t floppy_side_select(floppy_t *f, uint8_t head);
disk_err_t floppy_motor_on(floppy_t *f);
disk_err_t floppy_motor_off(floppy_t *f);
disk_err_t floppy_seek(floppy_t *f, uint8_t cylinder);
disk_err_t floppy_current_track(const floppy_t *f, uint8_t *cylinder);
disk_err_t floppy_at_track0(const floppy_t *f, bool *active);
disk_err_t floppy_disk_changed(floppy_t *f, bool *changed);
disk_err_t floppy_media_generation(floppy_t *f, uint32_t *generation);
disk_err_t floppy_write_protected(const floppy_t *f, bool *write_protected);
disk_err_t floppy_stats_reset(floppy_t *f);
disk_err_t floppy_stats(const floppy_t *f, floppy_stats_t *stats);

disk_err_t floppy_flux_begin(floppy_t *f, uint32_t expected_generation);
disk_result_t floppy_flux_read(floppy_t *f, floppy_pulse_t *pulses, size_t capacity);
disk_err_t floppy_flux_end(floppy_t *f);

disk_err_t floppy_read_track(floppy_t *f, uint32_t expected_generation,
                             track_t *track);
disk_err_t floppy_write_track(floppy_t *f, uint32_t expected_generation,
                              const track_t *track);

disk_device_t floppy_device(floppy_t *f);

#endif
