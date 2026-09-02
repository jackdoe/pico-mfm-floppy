#include "pio_sim.h"
#include "hardware/dma.h"
#include "../src/floppy.h"
#include "../src/mfm.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pio_sim_drive_t *g_drive;
static void pio_sim_load_track(void);
uint64_t pico_test_time_us;
static uint32_t tx_time_remainder;
static uint32_t rx_time_remainder;

typedef struct {
  bool read_active;
  bool write_active;
  bool write_sm_enabled;
  uint32_t *read_ring;
  uint32_t read_ring_mask;
  uint32_t read_ring_pos;
  const uint8_t *tx_source;
  uint32_t tx_length;
  uint32_t tx_position;
  uint8_t tx_fifo[8];
  uint8_t tx_head;
  uint8_t tx_count;
  uint32_t tx_emitted;
  dma_channel_hw_t hw;
} sim_dma_t;

static sim_dma_t sim_dma;

static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
         ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static uint16_t read_be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8u) | p[1]);
}

static uint32_t pio_sim_track_cycles(const pio_sim_track_t *track) {
  uint64_t cycles = 0;
  for (uint32_t i = 0; i < track->count; i++) cycles += track->deltas[i];
  if (cycles > UINT32_MAX) return UINT32_MAX;
  return cycles ? (uint32_t)cycles : 4800000u;
}

void pio_sim_init(pio_sim_drive_t *drive) {
  if (!drive) return;
  memset(drive, 0, sizeof(*drive));
  drive->last_index_value = true;
  drive->index_period_us = 200000u;
  drive->index_low_us = 2000u;
  if (g_drive && g_drive != drive) return;
  memset(&sim_dma, 0, sizeof(sim_dma));
  pio0->fdebug = 0;
  pio1->fdebug = 0;
  pico_test_time_us = 0;
  tx_time_remainder = 0;
  rx_time_remainder = 0;
}

void pio_sim_free(pio_sim_drive_t *drive) {
  if (!drive) return;
  bool installed = g_drive == drive;
  for (uint8_t cylinder = 0; cylinder < DISK_CYLINDERS; cylinder++) {
    for (uint8_t head = 0; head < DISK_HEADS; head++) {
      free(drive->tracks[cylinder][head].deltas);
    }
  }
  free(drive->write_capture);
  free(drive->alternate_track.deltas);
  memset(drive, 0, sizeof(*drive));
  if (installed) {
    memset(&sim_dma, 0, sizeof(sim_dma));
    g_drive = NULL;
  }
}

static void pio_sim_free_tracks(pio_sim_track_t tracks[DISK_CYLINDERS][DISK_HEADS]) {
  for (uint8_t cylinder = 0; cylinder < DISK_CYLINDERS; cylinder++) {
    for (uint8_t head = 0; head < DISK_HEADS; head++) {
      free(tracks[cylinder][head].deltas);
    }
  }
}

bool pio_sim_load_scp(pio_sim_drive_t *drive, uint8_t *data, size_t size) {
  if (!drive || !data || size < 0x10u + 168u * 4u || data[0] != 'S' ||
      data[1] != 'C' || data[2] != 'P' || data[5] == 0 || data[6] != 0 ||
      data[7] < DISK_TRACK_COUNT - 1u || data[7] >= 168u ||
      (data[8] & 1u) == 0 || data[9] != 0) {
    return false;
  }
  uint32_t checksum = 0;
  for (size_t i = 0x10; i < size; i++) checksum += data[i];
  if (checksum != read_le32(data + 0x0C)) return false;
  uint8_t revolutions = data[5];
  pio_sim_track_t loaded[DISK_CYLINDERS][DISK_HEADS] = {0};
  for (uint16_t scp_track = DISK_TRACK_COUNT; scp_track <= data[7]; scp_track++) {
    uint32_t header = read_le32(data + 0x10u + scp_track * 4u);
    if (header == 0 ||
        (uint64_t)header + 4u + (uint64_t)revolutions * 12u > size ||
        data[header] != 'T' || data[header + 1u] != 'R' ||
        data[header + 2u] != 'K' || data[header + 3u] != (uint8_t)scp_track) {
      return false;
    }
    for (uint8_t revolution_index = 0; revolution_index < revolutions;
         revolution_index++) {
      const uint8_t *entry = data + header + 4u + revolution_index * 12u;
      uint32_t duration = read_le32(entry);
      uint32_t count = read_le32(entry + 4u);
      uint32_t offset = read_le32(entry + 8u);
      if (duration == 0 || count == 0 || count > PIO_SIM_MAX_FLUX ||
          offset < 4u + (uint32_t)revolutions * 12u ||
          (uint64_t)header + offset + (uint64_t)count * 2u > size) {
        return false;
      }
    }
  }
  for (uint16_t scp_track = (uint16_t)data[7] + 1u; scp_track < 168u;
       scp_track++) {
    if (read_le32(data + 0x10u + scp_track * 4u) != 0) return false;
  }
  for (uint8_t cylinder = 0; cylinder < DISK_CYLINDERS; cylinder++) {
    for (uint8_t head = 0; head < DISK_HEADS; head++) {
      uint16_t scp_track = (uint16_t)(cylinder * DISK_HEADS + head);
      uint32_t table = 0x10u + scp_track * 4u;
      uint32_t header = read_le32(data + table);
      if (header == 0) goto fail;
      if ((uint64_t)header + 4u + (uint64_t)revolutions * 12u > size ||
          data[header] != 'T' || data[header + 1u] != 'R' ||
          data[header + 2u] != 'K' || data[header + 3u] != scp_track) {
        goto fail;
      }
      for (uint8_t revolution_index = 0; revolution_index < revolutions;
           revolution_index++) {
        const uint8_t *entry = data + header + 4u + revolution_index * 12u;
        uint32_t duration = read_le32(entry);
        uint32_t count = read_le32(entry + 4u);
        uint32_t offset = read_le32(entry + 8u);
        if (duration == 0 || count == 0 || count > PIO_SIM_MAX_FLUX ||
            offset < 4u + (uint32_t)revolutions * 12u ||
            (uint64_t)header + offset + (uint64_t)count * 2u > size) {
          goto fail;
        }
      }
      const uint8_t *revolution = data + header + 4u;
      uint32_t flux_count = read_le32(revolution + 4u);
      uint32_t flux_offset = read_le32(revolution + 8u);
      size_t allocation = (size_t)flux_count * sizeof(uint16_t);
      if (allocation / sizeof(uint16_t) != flux_count) goto fail;
      uint16_t *deltas = malloc(allocation);
      if (!deltas) goto fail;
      const uint8_t *flux = data + header + flux_offset;
      uint32_t output = 0;
      uint64_t accumulated = 0;
      for (uint32_t i = 0; i < flux_count; i++) {
        uint16_t value = read_be16(flux + i * 2u);
        if (value == 0) {
          if (accumulated > UINT64_MAX - 65536u) {
            free(deltas);
            goto fail;
          }
          accumulated += 65536u;
          continue;
        }
        if (accumulated > UINT64_MAX - value) {
          free(deltas);
          goto fail;
        }
        uint64_t total = accumulated + value;
        accumulated = 0;
        if (total > (UINT64_MAX - 2u) / 3u) {
          free(deltas);
          goto fail;
        }
        uint64_t scaled = total * 3u + 2u;
        uint64_t delta = scaled / 5u;
        if (delta == 0 || delta > UINT16_MAX) {
          free(deltas);
          goto fail;
        }
        deltas[output++] = (uint16_t)delta;
      }
      if (accumulated != 0 || output == 0) {
        free(deltas);
        goto fail;
      }
      loaded[cylinder][head].deltas = deltas;
      loaded[cylinder][head].count = output;
    }
  }
  pio_sim_free_tracks(drive->tracks);
  memcpy(drive->tracks, loaded, sizeof(loaded));
  return true;

fail:
  pio_sim_free_tracks(loaded);
  return false;
}

bool pio_sim_replace_track(pio_sim_drive_t *drive, const track_t *track) {
  if (!drive || !track || !disk_ch_valid(track->cylinder, track->head) ||
      track->valid != DISK_TRACK_VALID) {
    return false;
  }
  uint8_t *pulses = malloc(PIO_SIM_MAX_FLUX);
  if (!pulses) return false;
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, PIO_SIM_MAX_FLUX);
  mfm_encode_track(&encoder, track);
  if (encoder.stopped || encoder.pos == 0 || encoder.pos > UINT32_MAX) {
    free(pulses);
    return false;
  }
  uint16_t *deltas = malloc(encoder.pos * sizeof(*deltas));
  if (!deltas) {
    free(pulses);
    return false;
  }
  for (size_t i = 0; i < encoder.pos; i++) {
    deltas[i] = pulses[i] + MFM_PIO_OVERHEAD;
  }
  free(pulses);
  pio_sim_track_t *destination = &drive->tracks[track->cylinder][track->head];
  free(destination->deltas);
  destination->deltas = deltas;
  destination->count = (uint32_t)encoder.pos;
  if (drive == g_drive && drive->head_track == track->cylinder &&
      drive->head_side == track->head) {
    pio_sim_load_track();
  }
  return true;
}

void pio_sim_install(pio_sim_drive_t *drive, floppy_pins_t pins,
                     const uint32_t *ring, const uint32_t *consumer) {
  g_drive = drive;
  if (!drive) return;
  drive->pins = pins;
  drive->ring = ring;
  drive->consumer = consumer;
}

bool pio_sim_set_noise(pio_sim_drive_t *drive, flux_noise_config_t config) {
  return drive && flux_noise_configure(&drive->read_noise, &config);
}

static void pio_sim_load_track(void) {
  if (!g_drive) return;
  pio_sim_track_t *track = &g_drive->tracks[g_drive->head_track][g_drive->head_side];
  g_drive->read_buf = track->deltas;
  g_drive->read_count = track->count;
  g_drive->read_pos = 0;
  g_drive->counter = 0;
  g_drive->read_revolution = 0;
}

static void pio_sim_commit_write(void) {
  if (!g_drive || !g_drive->write_capture ||
      g_drive->write_capture_count == 0 ||
      g_drive->write_capture_count > g_drive->write_capture_capacity) {
    return;
  }
  pio_sim_track_t *track = &g_drive->tracks[g_drive->head_track][g_drive->head_side];
  if (g_drive->fault_writes_remaining > 0) {
    g_drive->fault_writes_remaining--;
    g_drive->torn_writes++;
    uint32_t prefix = g_drive->tear_after_pulses
                          ? g_drive->tear_after_pulses
                          : g_drive->write_capture_count / 2u;
    if (prefix > g_drive->write_capture_count) prefix = g_drive->write_capture_count;
    uint32_t old_start = prefix < track->count ? prefix : track->count;
    uint32_t suffix = track->count - old_start;
    if (prefix > UINT32_MAX - suffix) return;
    uint32_t count = prefix + suffix;
    if (count == 0) return;
    size_t allocation = (size_t)count * sizeof(uint16_t);
    if (allocation / sizeof(uint16_t) != count) return;
    uint16_t *deltas = malloc(allocation);
    if (!deltas) return;
    for (uint32_t i = 0; i < prefix; i++) {
      deltas[i] = g_drive->write_capture[i] + MFM_PIO_OVERHEAD;
    }
    if (suffix != 0) {
      memcpy(deltas + prefix, track->deltas + old_start,
             (size_t)suffix * sizeof(*deltas));
    }
    free(track->deltas);
    track->deltas = deltas;
    track->count = count;
    pio_sim_load_track();
    return;
  }
  uint8_t duplicate_pulses[8192];
  size_t duplicate_count = 0;
  if (g_drive->inject_stale_duplicate) {
    uint8_t stale[DISK_SECTOR_SIZE];
    memset(stale, 0xE7, sizeof(stale));
    mfm_encode_t encoder;
    mfm_encode_init(&encoder, duplicate_pulses, sizeof(duplicate_pulses));
    mfm_encode_gap(&encoder, 22);
    mfm_encode_sector(&encoder, g_drive->head_track, g_drive->head_side, 0, stale);
    mfm_encode_gap(&encoder, 22);
    duplicate_count = encoder.pos;
  }
  if (duplicate_count > UINT32_MAX - g_drive->write_capture_count) return;
  uint32_t count = g_drive->write_capture_count + (uint32_t)duplicate_count;
  if (count == 0) return;
  size_t allocation = (size_t)count * sizeof(uint16_t);
  if (allocation / sizeof(uint16_t) != count) return;
  uint16_t *deltas = malloc(allocation);
  if (!deltas) return;
  for (uint32_t i = 0; i < g_drive->write_capture_count; i++) {
    deltas[i] = g_drive->write_capture[i] + MFM_PIO_OVERHEAD;
  }
  for (size_t i = 0; i < duplicate_count; i++) {
    deltas[g_drive->write_capture_count + i] =
        duplicate_pulses[i] + MFM_PIO_OVERHEAD;
  }
  free(track->deltas);
  track->deltas = deltas;
  track->count = count;
  pio_sim_load_track();
}

void gpio_init(uint pin) {
  (void)pin;
}

void gpio_deinit(uint pin) {
  (void)pin;
  if (g_drive) g_drive->gpio_deinits++;
}

void gpio_set_dir(uint pin, bool output) {
  if (!g_drive) return;
  const floppy_pins_t *pins = &g_drive->pins;
  if (pin == pins->direction) {
    g_drive->step_direction_inward = output;
  } else if (pin == pins->step && output) {
    g_drive->step_pulses++;
    bool moved = false;
    if (g_drive->step_direction_inward && g_drive->head_track + 1u < DISK_CYLINDERS) {
      g_drive->head_track++;
      moved = true;
    } else if (!g_drive->step_direction_inward && g_drive->head_track > 0) {
      g_drive->head_track--;
      moved = true;
    }
    if (moved && !g_drive->disk_change_stuck) g_drive->disk_changed = false;
    pio_sim_load_track();
  } else if (pin == pins->side_select) {
    uint8_t head = output ? 1u : 0u;
    if (head != g_drive->head_side) {
      g_drive->head_side = head;
      pio_sim_load_track();
    }
  } else if (pin == pins->drive_select) {
    g_drive->selected = output;
  } else if (pin == pins->motor_enable) {
    g_drive->motor_on = output;
  } else if (pin == pins->density) {
    g_drive->density_hd = output;
  } else if (pin == pins->write_gate) {
    if (output && !g_drive->write_gate_active) {
      g_drive->write_gate_active = true;
      g_drive->write_gate_assertions++;
      g_drive->write_capture_count = 0;
      g_drive->write_elapsed_cycles = 0;
      pio_sim_track_t *track =
          &g_drive->tracks[g_drive->head_track][g_drive->head_side];
      g_drive->write_revolution_cycles = g_drive->write_revolution_override
                                             ? g_drive->write_revolution_override
                                             : pio_sim_track_cycles(track);
      g_drive->write_started_at_index = !g_drive->last_index_value;
      g_drive->write_ended_at_index = false;
    } else if (!output && g_drive->write_gate_active) {
      g_drive->write_gate_active = false;
      g_drive->write_gate_deassertions++;
      g_drive->write_ended_at_index =
          g_drive->write_elapsed_cycles >= g_drive->write_revolution_cycles;
      pio_sim_commit_write();
    }
  }
}

void gpio_pull_up(uint pin) {
  (void)pin;
}

void gpio_disable_pulls(uint pin) {
  (void)pin;
}

void gpio_put(uint pin, bool value) {
  (void)pin;
  (void)value;
}

bool gpio_get(uint pin) {
  if (!g_drive) return true;
  const floppy_pins_t *pins = &g_drive->pins;
  if (pin == pins->track0) {
    return g_drive->track0_missing || g_drive->head_track != 0;
  }
  if (pin == pins->disk_change) {
    return !g_drive->selected || !g_drive->disk_changed;
  }
  if (pin == pins->write_protect) return !g_drive->write_protected;
  if (pin != pins->index) return true;
  if (!g_drive->write_gate_active) pico_test_time_us += 1000u;
  bool value;
  if (g_drive->index_stuck) {
    value = g_drive->index_value;
  } else if (g_drive->write_gate_active) {
    value = g_drive->write_elapsed_cycles != 0 &&
            g_drive->write_elapsed_cycles < g_drive->write_revolution_cycles;
  } else {
    g_drive->index_poll_count++;
    uint32_t period = g_drive->index_period_us ? g_drive->index_period_us : 200000u;
    uint32_t phase = (uint32_t)(pico_test_time_us % period);
    value = phase >= g_drive->index_low_us;
  }
  g_drive->last_index_value = value;
  return value;
}

bool pio_can_add_program(PIO pio, const pio_program_t *program) {
  (void)pio;
  (void)program;
  return !g_drive || !g_drive->fail_pio_capacity;
}

int pio_add_program(PIO pio, const pio_program_t *program) {
  (void)pio;
  (void)program;
  if (g_drive) {
    g_drive->program_add_calls++;
    if (g_drive->fail_program_add_call > 0 &&
        g_drive->program_add_calls == (uint32_t)g_drive->fail_program_add_call) {
      return -1;
    }
    g_drive->programs_loaded++;
  }
  return 0;
}

void pio_remove_program(PIO pio, const pio_program_t *program, uint offset) {
  (void)pio;
  (void)program;
  (void)offset;
  if (g_drive && g_drive->programs_loaded > 0) g_drive->programs_loaded--;
}

int pio_claim_unused_sm(PIO pio, bool required) {
  (void)pio;
  (void)required;
  if (g_drive) {
    g_drive->sm_claim_calls++;
    if (g_drive->fail_sm_claim_call > 0 &&
        g_drive->sm_claim_calls == (uint32_t)g_drive->fail_sm_claim_call) {
      return -1;
    }
    g_drive->sms_claimed++;
  }
  return 0;
}

void pio_sm_unclaim(PIO pio, uint sm) {
  (void)pio;
  (void)sm;
  if (g_drive && g_drive->sms_claimed > 0) g_drive->sms_claimed--;
}

int pio_sm_init(PIO pio, uint sm, uint offset, const pio_sm_config *config) {
  (void)pio;
  (void)sm;
  (void)offset;
  (void)config;
  return 0;
}

void pio_sm_exec(PIO pio, uint sm, uint instruction) {
  (void)sm;
  if (g_drive && pio && pio->id == 0 && (instruction & 0x10000u) != 0) {
    g_drive->read_restart_jumps++;
  }
}

void pio_sm_restart(PIO pio, uint sm) {
  (void)pio;
  (void)sm;
}

void pio_sm_clear_fifos(PIO pio, uint sm) {
  (void)sm;
  if (!g_drive) return;
  if (pio && pio->id == 0) {
    pio_sim_load_track();
  } else {
    sim_dma.tx_head = 0;
    sim_dma.tx_count = 0;
  }
}

void pio_sm_set_enabled(PIO pio, uint sm, bool enabled) {
  (void)sm;
  if (pio && pio->id == 1) sim_dma.write_sm_enabled = enabled;
}

void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint32_t values, uint32_t mask) {
  (void)pio;
  (void)sm;
  (void)values;
  (void)mask;
}

void pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin, uint count,
                                    bool output) {
  (void)pio;
  (void)sm;
  (void)pin;
  (void)count;
  (void)output;
}

void pio_gpio_init(PIO pio, uint pin) {
  (void)pio;
  (void)pin;
}

uint pio_get_gpio_base(PIO pio) {
  (void)pio;
  return g_drive ? g_drive->pio_gpio_base : 0;
}

bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm) {
  (void)pio;
  (void)sm;
  return !g_drive || !g_drive->read_buf || g_drive->read_count == 0;
}

static uint16_t pio_sim_next_sample(void) {
  if (!g_drive || !g_drive->read_buf || g_drive->read_count == 0) {
    bool index = g_drive && g_drive->index_stuck ? g_drive->index_value : true;
    return (uint16_t)((0x7FFFu << 1) | index);
  }
  if (g_drive->read_pos >= g_drive->read_count) {
    g_drive->read_revolution++;
    if (g_drive->alternate_read && g_drive->alternate_track.count != 0 &&
        (g_drive->read_revolution & 1u)) {
      g_drive->read_buf = g_drive->alternate_track.deltas;
      g_drive->read_count = g_drive->alternate_track.count;
    } else {
      pio_sim_track_t *track =
          &g_drive->tracks[g_drive->head_track][g_drive->head_side];
      g_drive->read_buf = track->deltas;
      g_drive->read_count = track->count;
    }
    g_drive->read_pos = 0;
  }
  g_drive->flux_sample_reads++;
  if (g_drive->disk_change_after_samples != 0 &&
      g_drive->flux_sample_reads >= g_drive->disk_change_after_samples) {
    g_drive->disk_changed = true;
  }
  uint16_t delta = flux_noise_apply(
      &g_drive->read_noise, g_drive->read_buf[g_drive->read_pos++]);
  g_drive->counter -= delta;
  rx_time_remainder += delta;
  pico_test_time_us += rx_time_remainder / 24u;
  rx_time_remainder %= 24u;
  bool index = true;
  if (g_drive->index_stuck) {
    index = g_drive->index_value;
  } else if (g_drive->read_pos == g_drive->read_count) {
    index = false;
  }
  return (uint16_t)(((g_drive->counter & 0x7FFFu) << 1) | index);
}

uint32_t pio_sm_get_blocking(PIO pio, uint sm) {
  (void)pio;
  (void)sm;
  uint16_t low = pio_sim_next_sample();
  uint16_t high = pio_sim_next_sample();
  return ((uint32_t)high << 16) | low;
}

bool pio_sm_is_tx_fifo_empty(PIO pio, uint sm) {
  (void)pio;
  (void)sm;
  return sim_dma.tx_count == 0;
}

static void pio_sim_capture(uint8_t pulse) {
  if (!g_drive || !g_drive->write_gate_active) return;
  if (g_drive->write_capture_count == g_drive->write_capture_capacity) {
    uint32_t capacity = g_drive->write_capture_capacity
                            ? g_drive->write_capture_capacity * 2u
                            : 4096u;
    uint8_t *capture = realloc(g_drive->write_capture, capacity);
    if (!capture) return;
    g_drive->write_capture = capture;
    g_drive->write_capture_capacity = capacity;
  }
  g_drive->write_capture[g_drive->write_capture_count++] = pulse;
  if (g_drive->write_protect_after_pulses != 0 &&
      g_drive->write_capture_count >= g_drive->write_protect_after_pulses) {
    g_drive->write_protected = true;
  }
  if (g_drive->disk_change_after_pulses != 0 &&
      g_drive->write_capture_count >= g_drive->disk_change_after_pulses) {
    g_drive->disk_changed = true;
  }
  uint32_t delta = pulse + MFM_PIO_OVERHEAD;
  if (g_drive->write_elapsed_cycles > UINT32_MAX - delta) {
    g_drive->write_elapsed_cycles = UINT32_MAX;
  } else {
    g_drive->write_elapsed_cycles += delta;
  }
  tx_time_remainder += delta;
  pico_test_time_us += tx_time_remainder / 24u;
  tx_time_remainder %= 24u;
}

void pio_sm_put_blocking(PIO pio, uint sm, uint32_t data) {
  (void)pio;
  (void)sm;
  pio_sim_capture(data & 0xFFu);
}

static void sim_tx_fill(void) {
  if (!sim_dma.write_active || !g_drive || g_drive->tx_stall) return;
  while (sim_dma.tx_count < sizeof(sim_dma.tx_fifo) &&
         sim_dma.tx_position < sim_dma.tx_length) {
    if (!g_drive->ring) {
      g_drive->dma_source_overread = true;
      break;
    }
    uint8_t tail = (sim_dma.tx_head + sim_dma.tx_count) % sizeof(sim_dma.tx_fifo);
    sim_dma.tx_fifo[tail] = sim_dma.tx_source[sim_dma.tx_position++];
    sim_dma.tx_count++;
    sim_dma.hw.transfer_count--;
  }
  if (sim_dma.tx_position == sim_dma.tx_length) sim_dma.write_active = false;
}

static void sim_tx_advance(void) {
  if (sim_dma.write_sm_enabled && sim_dma.tx_count > 0) {
    uint8_t pulse = sim_dma.tx_fifo[sim_dma.tx_head];
    sim_dma.tx_head = (sim_dma.tx_head + 1u) % sizeof(sim_dma.tx_fifo);
    sim_dma.tx_count--;
    pio_sim_capture(pulse);
    sim_dma.tx_emitted++;
  }
  uint32_t transient_after = g_drive && g_drive->tx_transient_stall_after_pulses
                                 ? g_drive->tx_transient_stall_after_pulses
                                 : 64u;
  if (g_drive && g_drive->tx_transient_stall &&
      sim_dma.tx_emitted >= transient_after) {
    pio1->fdebug |= 1u << PIO_FDEBUG_TXSTALL_LSB;
    g_drive->tx_transient_stall = false;
  }
  sim_tx_fill();
  uint32_t underrun_after = g_drive && g_drive->tx_force_underrun_after_pulses
                                ? g_drive->tx_force_underrun_after_pulses
                                : 64u;
  if (g_drive && g_drive->tx_force_underrun &&
      (sim_dma.tx_emitted >= underrun_after || !sim_dma.write_active)) {
    sim_dma.write_active = false;
    sim_dma.tx_head = 0;
    sim_dma.tx_count = 0;
    if (sim_dma.write_sm_enabled) {
      pio1->fdebug |= 1u << PIO_FDEBUG_TXSTALL_LSB;
    }
    if (!g_drive->tx_force_underrun_repeat) g_drive->tx_force_underrun = false;
  }
}

int dma_claim_unused_channel(bool required) {
  (void)required;
  if (g_drive && g_drive->fail_dma_claim) return -1;
  if (g_drive) g_drive->dma_channels_claimed++;
  return 0;
}

void dma_channel_unclaim(uint channel) {
  (void)channel;
  if (g_drive && g_drive->dma_channels_claimed > 0) {
    g_drive->dma_channels_claimed--;
  }
}

void dma_channel_configure(uint channel, const dma_channel_config *config,
                           volatile void *write_addr, const volatile void *read_addr,
                           uint transfer_count, bool trigger) {
  (void)channel;
  (void)trigger;
  if (g_drive) g_drive->dma_high_priority = config->high_priority;
  if (config->ring_write) {
    sim_dma.read_active = true;
    sim_dma.write_active = false;
    sim_dma.read_ring = (uint32_t *)write_addr;
    sim_dma.read_ring_mask = ((1u << config->ring_bits) / 4u) - 1u;
    sim_dma.read_ring_pos = 0;
    sim_dma.hw.transfer_count = transfer_count;
    return;
  }
  sim_dma.read_active = false;
  sim_dma.write_active = transfer_count != 0;
  sim_dma.tx_source = (const uint8_t *)read_addr;
  sim_dma.tx_length = transfer_count;
  sim_dma.tx_position = 0;
  sim_dma.tx_emitted = 0;
  sim_dma.hw.transfer_count = transfer_count;
  if (!g_drive) return;
  g_drive->dma_tx_configurations++;
  if (g_drive->disk_change_on_tx_configure) g_drive->disk_changed = true;
  if (g_drive->write_protect_on_tx_configure) g_drive->write_protected = true;
  if (g_drive->dma_tx_min_count == 0 ||
      transfer_count < g_drive->dma_tx_min_count) {
    g_drive->dma_tx_min_count = transfer_count;
  }
  if (transfer_count > g_drive->dma_tx_max_count) {
    g_drive->dma_tx_max_count = transfer_count;
  }
  if (g_drive->ring) {
    uintptr_t base = (uintptr_t)g_drive->ring;
    uintptr_t source = (uintptr_t)read_addr;
    bool in_ring = source >= base && source < base + FLOPPY_FLUX_RING_BYTES;
    uintptr_t offset = in_ring ? source - base : FLOPPY_FLUX_RING_BYTES;
    bool owned_half = in_ring && offset % FLOPPY_TX_HALF_BYTES == 0 &&
                      transfer_count <= FLOPPY_TX_HALF_BYTES;
    if (!owned_half) g_drive->dma_source_overread = true;
  }
}

bool dma_channel_is_busy(uint channel) {
  (void)channel;
  if (sim_dma.write_active) sim_tx_advance();
  return sim_dma.read_active || sim_dma.write_active;
}

void dma_channel_abort(uint channel) {
  (void)channel;
  sim_dma.read_active = false;
  sim_dma.write_active = false;
  sim_dma.hw.transfer_count = 0;
}

dma_channel_hw_t *dma_channel_hw_addr(uint channel) {
  (void)channel;
  if (sim_dma.read_active && g_drive && g_drive->read_buf &&
      g_drive->read_count > 0 && sim_dma.hw.transfer_count > 0 &&
      g_drive->consumer) {
    if (g_drive->rx_pio_stall) {
      pio0->fdebug |= 1u << PIO_FDEBUG_RXSTALL_LSB;
      if (!g_drive->rx_pio_stall_repeat) g_drive->rx_pio_stall = false;
    }
    uint32_t words = g_drive->rx_burst_words;
    if (words == 0 &&
        sim_dma.read_ring_pos - *g_drive->consumer < 4u) {
      words = 1;
    }
    if (words > sim_dma.hw.transfer_count) words = sim_dma.hw.transfer_count;
    for (uint32_t i = 0; i < words; i++) {
      uint16_t low = pio_sim_next_sample();
      uint16_t high = pio_sim_next_sample();
      sim_dma.read_ring[sim_dma.read_ring_pos & sim_dma.read_ring_mask] =
          ((uint32_t)high << 16) | low;
      sim_dma.read_ring_pos++;
      sim_dma.hw.transfer_count--;
    }
    if (g_drive->rx_burst_words != 0 && !g_drive->rx_burst_repeat) {
      g_drive->rx_burst_words = 0;
    }
  } else if (sim_dma.write_active) {
    sim_tx_fill();
  }
  return &sim_dma.hw;
}

static pio_hw_t pio0_hw = {.id = 0};
static pio_hw_t pio1_hw = {.id = 1};
PIO pio0 = &pio0_hw;
PIO pio1 = &pio1_hw;

const pio_program_t flux_read_program = {0};
const pio_program_t flux_write_program = {0};

int flux_read_program_init(PIO pio, uint sm, uint offset, uint pin, uint index_pin) {
  (void)pio;
  (void)sm;
  (void)offset;
  (void)pin;
  (void)index_pin;
  return g_drive && g_drive->fail_read_program_init ? -1 : 0;
}

int flux_write_program_init(PIO pio, uint sm, uint offset, uint pin) {
  (void)pio;
  (void)sm;
  (void)offset;
  (void)pin;
  return g_drive && g_drive->fail_write_program_init ? -1 : 0;
}

