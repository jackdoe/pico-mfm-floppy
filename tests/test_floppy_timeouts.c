#include "sim_floppy.h"

static pio_sim_drive_t drive;
static floppy_t floppy;
extern uint64_t pico_test_time_us;

static disk_err_t read_track0(uint32_t expected_generation) {
  track_t track = {.cylinder = 0, .head = 0};
  return floppy_read_track(&floppy, expected_generation, &track);
}

static void install_flux(void) {
  track_t track;
  sim_fill_track(&track, 0, 0, 0);
  sim_install_track(&drive, &track);
}

TEST(test_read_track_times_out_without_flux) {
  sim_setup(&drive, &floppy);
  ASSERT_EQ(read_track0(sim_generation(&floppy)), DISK_ERR_TIMEOUT);
}

TEST(test_continuous_flux_with_stuck_index_is_bounded) {
  sim_setup(&drive, &floppy);
  install_flux();
  ASSERT_EQ(floppy_motor_on(&floppy), DISK_OK);
  drive.index_stuck = true;
  drive.index_value = true;
  uint32_t before = drive.flux_sample_reads;
  ASSERT_EQ(read_track0(sim_generation(&floppy)), DISK_ERR_TIMEOUT);
  ASSERT(drive.flux_sample_reads > before);
  ASSERT(drive.flux_sample_reads <= 4000000u);
}

TEST(test_write_track_times_out_without_index_before_gate) {
  sim_setup(&drive, &floppy);
  drive.index_stuck = true;
  drive.index_value = false;
  track_t track;
  sim_fill_track(&track, 0, 0, 0);
  uint32_t expected_generation = sim_generation(&floppy);
  ASSERT_EQ(floppy_write_track(&floppy, expected_generation, &track), DISK_ERR_TIMEOUT);
  ASSERT_EQ(drive.write_gate_assertions, 0);
  ASSERT_EQ(drive.write_capture_count, 0);
}

TEST(test_stale_generation_rejects_read_without_flux) {
  sim_setup(&drive, &floppy);
  install_flux();
  ASSERT_EQ(floppy_select(&floppy, true), DISK_OK);
  uint32_t expected_generation = sim_generation(&floppy);
  drive.disk_changed = true;
  uint32_t before = drive.flux_sample_reads;
  ASSERT_EQ(read_track0(expected_generation),
            DISK_ERR_MEDIA_CHANGED);
  ASSERT_EQ(drive.flux_sample_reads, before);
}

TEST(test_absolute_deadline_survives_millisecond_wrap) {
  sim_setup(&drive, &floppy);
  pico_test_time_us = ((uint64_t)UINT32_MAX - 2u) * 1000u;
  uint64_t before = pico_test_time_us;
  ASSERT_EQ(read_track0(sim_generation(&floppy)),
            DISK_ERR_TIMEOUT);
  ASSERT(pico_test_time_us - before <= 5100000u);
}

int main(void) {
  pio_sim_init(&drive);
  printf("=== Floppy Deadline Tests ===\n\n");
  RUN_TEST(test_read_track_times_out_without_flux);
  RUN_TEST(test_continuous_flux_with_stuck_index_is_bounded);
  RUN_TEST(test_write_track_times_out_without_index_before_gate);
  RUN_TEST(test_stale_generation_rejects_read_without_flux);
  RUN_TEST(test_absolute_deadline_survives_millisecond_wrap);
  ASSERT_EQ(floppy_deinit(&floppy), DISK_OK);
  pio_sim_free(&drive);
  TEST_RESULTS();
}
