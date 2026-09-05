#ifndef SIM_FLOPPY_H
#define SIM_FLOPPY_H

#include "test.h"
#include "pio_sim.h"
#include "../src/floppy.h"
#include "../src/mfm.h"
#include <stdlib.h>
#include <string.h>

static inline floppy_pins_t sim_test_pins(void) {
  return (floppy_pins_t){
      .index = 1, .track0 = 2, .write_protect = 3, .read_data = 4,
      .disk_change = 5, .drive_select = 6, .motor_enable = 7,
      .direction = 8, .step = 9, .write_data = 10, .write_gate = 11,
      .side_select = 12, .density = 13,
  };
}

static inline uint32_t sim_generation(floppy_t *floppy) {
  uint32_t value;
  ASSERT_EQ(floppy_media_generation(floppy, &value), DISK_OK);
  return value;
}

static inline disk_err_t sim_flux_next(floppy_t *floppy, uint16_t *delta, bool *index) {
  floppy_pulse_t pulse;
  disk_result_t result = floppy_flux_read(floppy, &pulse, 1);
  if (result.error != DISK_OK) return result.error;
  *delta = pulse.delta;
  *index = pulse.index;
  return DISK_OK;
}

static inline void sim_fill_track(track_t *track, uint8_t cylinder, uint8_t head,
                                  uint8_t salt) {
  memset(track, 0, sizeof(*track));
  track->cylinder = cylinder;
  track->head = head;
  track->valid = DISK_TRACK_VALID;
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    for (size_t byte = 0; byte < DISK_SECTOR_SIZE; byte++) {
      track->data[sector][byte] =
          (uint8_t)((size_t)salt + (size_t)sector * 17u + byte);
    }
  }
}

static inline size_t sim_track_pulses(const track_t *track, uint8_t *pulses,
                                      size_t capacity) {
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, capacity);
  mfm_encode_track(&encoder, track);
  ASSERT(!encoder.stopped);
  return encoder.pos;
}

static inline uint32_t sim_track_load(pio_sim_track_t *track, const uint8_t *pulses,
                                      size_t count) {
  ASSERT(count != 0 && count <= PIO_SIM_MAX_FLUX);
  free(track->deltas);
  track->deltas = malloc(count * sizeof(*track->deltas));
  ASSERT(track->deltas != NULL);
  track->count = (uint32_t)count;
  uint64_t cycles = 0;
  for (size_t i = 0; i < count; i++) {
    track->deltas[i] = (uint16_t)(pulses[i]);
    cycles += track->deltas[i];
  }
  ASSERT(cycles < UINT32_MAX - 48000u);
  return (uint32_t)cycles;
}

static inline uint32_t sim_install_pulses(pio_sim_drive_t *drive, uint8_t cylinder,
                                          uint8_t head, const uint8_t *pulses,
                                          size_t count) {
  return sim_track_load(&drive->tracks[cylinder][head], pulses, count);
}

static inline uint32_t sim_install_track(pio_sim_drive_t *drive,
                                         const track_t *track) {
  static uint8_t pulses[PIO_SIM_MAX_FLUX];
  size_t count = sim_track_pulses(track, pulses, sizeof(pulses));
  return sim_install_pulses(drive, track->cylinder, track->head, pulses, count);
}

static inline void sim_setup(pio_sim_drive_t *drive, floppy_t *floppy) {
  floppy_deinit(floppy);
  pio_sim_free(drive);
  pio_sim_init(drive);
  pio_sim_install(drive, sim_test_pins(), floppy->flux_ring, &floppy->ring_cpu);
  memset(floppy, 0, sizeof(*floppy));
  ASSERT_EQ(floppy_init(floppy, sim_test_pins()), DISK_OK);
}

#endif
