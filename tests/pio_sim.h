#ifndef PIO_SIM_H
#define PIO_SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../src/block.h"
#include "flux_noise.h"

#define PIO_SIM_MAX_FLUX 200000u

typedef struct {
  uint16_t *deltas;
  uint32_t count;
} pio_sim_track_t;

typedef struct {
  pio_sim_track_t tracks[DISK_CYLINDERS][DISK_HEADS];
  pio_sim_track_t alternate_track;
  bool alternate_read;
  uint32_t read_revolution;
  uint8_t head_track;
  uint8_t head_side;
  bool motor_on;
  bool selected;
  bool write_protected;
  uint32_t write_protect_after_pulses;
  uint32_t disk_change_after_pulses;
  uint32_t disk_change_after_samples;
  bool step_direction_inward;
  bool density_hd;
  bool disk_changed;
  uint16_t *read_buf;
  uint32_t read_count;
  uint32_t read_pos;
  uint16_t counter;
  uint8_t *write_capture;
  uint32_t write_capture_count;
  uint32_t write_capture_capacity;
  uint32_t write_elapsed_cycles;
  uint32_t write_revolution_cycles;
  uint32_t write_revolution_override;
  uint32_t write_gate_assertions;
  uint32_t write_gate_deassertions;
  uint32_t index_poll_count;
  uint32_t index_period_us;
  uint32_t index_low_us;
  uint32_t flux_sample_reads;
  uint32_t dma_tx_configurations;
  uint32_t dma_tx_min_count;
  uint32_t dma_tx_max_count;
  int fault_writes_remaining;
  uint32_t tear_after_pulses;
  uint32_t torn_writes;
  bool index_stuck;
  bool index_value;
  bool last_index_value;
  bool write_gate_active;
  bool write_started_at_index;
  bool write_ended_at_index;
  bool tx_stall;
  bool tx_force_underrun;
  bool tx_force_underrun_repeat;
  uint32_t tx_force_underrun_after_pulses;
  bool tx_transient_stall;
  uint32_t tx_transient_stall_after_pulses;
  uint32_t rx_burst_words;
  bool rx_burst_repeat;
  bool rx_pio_stall;
  bool rx_pio_stall_repeat;
  bool dma_source_overread;
  bool dma_high_priority;
  bool inject_stale_duplicate;
  bool fail_pio_capacity;
  int fail_program_add_call;
  int fail_sm_claim_call;
  bool fail_dma_claim;
  bool fail_spinlock_claim;
  bool fail_timer_create;
  bool fail_read_program_init;
  bool fail_write_program_init;
  uint32_t program_add_calls;
  uint32_t sm_claim_calls;
  uint32_t programs_loaded;
  uint32_t sms_claimed;
  uint32_t dma_channels_claimed;
  uint32_t spin_locks_claimed;
  uint32_t timers_active;
  uint32_t gpio_deinits;
  uint32_t read_restart_jumps;
  uint32_t pio_gpio_base;
  flux_noise_t read_noise;
} pio_sim_drive_t;

bool pio_sim_replace_track(pio_sim_drive_t *drive, const track_t *track);
bool pio_sim_fire_timer(pio_sim_drive_t *drive);
bool pio_sim_set_noise(pio_sim_drive_t *drive, flux_noise_config_t config);

void pio_sim_init(pio_sim_drive_t *drive);
void pio_sim_free(pio_sim_drive_t *drive);
bool pio_sim_load_scp(pio_sim_drive_t *drive, uint8_t *scp_data, size_t scp_size);
void pio_sim_install(pio_sim_drive_t *drive);

#endif
