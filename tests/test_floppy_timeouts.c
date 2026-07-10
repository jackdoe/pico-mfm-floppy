#include "test.h"
#include "pio_sim.h"
#include "../src/floppy.h"
#include "../src/mfm_encode.h"

floppy_t *pio_sim_floppy_ref;

static pio_sim_drive_t drive;
static floppy_t floppy;
extern uint64_t pico_test_time_us;

static floppy_pins_t test_pins(void) {
  return (floppy_pins_t){
      .index = 1, .track0 = 2, .write_protect = 3, .read_data = 4,
      .disk_change = 5, .drive_select = 6, .motor_enable = 7,
      .direction = 8, .step = 9, .write_data = 10, .write_gate = 11,
      .side_select = 12, .density = 13,
  };
}

static uint32_t generation(void) {
  uint32_t value;
  ASSERT_EQ(floppy_media_generation(&floppy, &value), BLOCK_OK);
  return value;
}

static void setup_floppy(void) {
  floppy_deinit(&floppy);
  pio_sim_free(&drive);
  pio_sim_init(&drive);
  pio_sim_install(&drive);
  memset(&floppy, 0, sizeof(floppy));
  pio_sim_floppy_ref = &floppy;
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_OK);
}

static void fill_track(track_t *track) {
  memset(track, 0, sizeof(*track));
  track->cylinder = 0;
  track->head = 0;
  track->valid = DISK_TRACK_VALID;
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    memset(track->data[sector], sector, DISK_SECTOR_SIZE);
  }
}

static void install_flux(void) {
  static uint8_t pulses[200000];
  track_t track;
  fill_track(&track);
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_track(&encoder, &track);
  pio_sim_track_t *sim_track = &drive.tracks[0][0];
  sim_track->deltas = malloc(encoder.pos * sizeof(*sim_track->deltas));
  ASSERT(sim_track->deltas != NULL);
  ASSERT(encoder.pos <= UINT32_MAX);
  sim_track->count = (uint32_t)encoder.pos;
  for (size_t i = 0; i < encoder.pos; i++) {
    sim_track->deltas[i] = pulses[i] + MFM_PIO_OVERHEAD;
  }
}

TEST(test_read_sector_times_out_without_flux) {
  setup_floppy();
  uint8_t data[DISK_SECTOR_SIZE];
  ASSERT_EQ(floppy_read_sector(&floppy, generation(),
                               0, 0, 0, data), BLOCK_ERR_TIMEOUT);
}

TEST(test_continuous_flux_with_stuck_index_is_bounded) {
  setup_floppy();
  install_flux();
  ASSERT_EQ(floppy_motor_on(&floppy), BLOCK_OK);
  drive.index_stuck = true;
  drive.index_value = true;
  uint8_t data[DISK_SECTOR_SIZE];
  uint32_t before = drive.flux_sample_reads;
  ASSERT_EQ(floppy_read_sector(&floppy, generation(),
                               0, 0, 0, data), BLOCK_ERR_TIMEOUT);
  ASSERT(drive.flux_sample_reads > before);
  ASSERT(drive.flux_sample_reads <= 4000000u);
}

TEST(test_write_track_times_out_without_index_before_gate) {
  setup_floppy();
  drive.index_stuck = true;
  drive.index_value = false;
  track_t track;
  fill_track(&track);
  uint32_t expected_generation = generation();
  ASSERT_EQ(floppy_write_track(&floppy, expected_generation, &track), BLOCK_ERR_TIMEOUT);
  ASSERT_EQ(drive.write_gate_assertions, 0);
  ASSERT_EQ(drive.write_capture_count, 0);
}

TEST(test_stale_generation_rejects_read_without_flux) {
  setup_floppy();
  install_flux();
  uint32_t expected_generation = generation();
  drive.disk_changed = true;
  uint8_t data[DISK_SECTOR_SIZE];
  uint32_t before = drive.flux_sample_reads;
  ASSERT_EQ(floppy_read_sector(&floppy, expected_generation, 0, 0, 0, data),
            BLOCK_ERR_MEDIA_CHANGED);
  ASSERT_EQ(drive.flux_sample_reads, before);
}

TEST(test_absolute_deadline_survives_millisecond_wrap) {
  setup_floppy();
  pico_test_time_us = ((uint64_t)UINT32_MAX - 2u) * 1000u;
  uint64_t before = pico_test_time_us;
  uint8_t data[DISK_SECTOR_SIZE];
  ASSERT_EQ(floppy_read_sector(&floppy, generation(), 0, 0, 0, data),
            BLOCK_ERR_TIMEOUT);
  ASSERT(pico_test_time_us - before <= 5100000u);
}

int main(void) {
  pio_sim_init(&drive);
  printf("=== Floppy Deadline Tests ===\n\n");
  RUN_TEST(test_read_sector_times_out_without_flux);
  RUN_TEST(test_continuous_flux_with_stuck_index_is_bounded);
  RUN_TEST(test_write_track_times_out_without_index_before_gate);
  RUN_TEST(test_stale_generation_rejects_read_without_flux);
  RUN_TEST(test_absolute_deadline_survives_millisecond_wrap);
  ASSERT_EQ(floppy_deinit(&floppy), BLOCK_OK);
  pio_sim_free(&drive);
  TEST_RESULTS();
}
