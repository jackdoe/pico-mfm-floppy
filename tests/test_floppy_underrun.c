#include "sim_floppy.h"

static pio_sim_drive_t drive;
static floppy_t floppy;

static void setup_floppy(void) {
  sim_setup(&drive, &floppy);
  drive.write_revolution_override = 4800000u;
}

TEST(test_tx_starvation_after_gate_is_explicit_underrun) {
  setup_floppy();
  drive.tx_force_underrun = true;
  drive.tx_force_underrun_repeat = true;
  track_t track;
  sim_fill_track(&track, 0, 0, 0);
  uint32_t expected_generation = sim_generation(&floppy);
  ASSERT_EQ(floppy_write_track(&floppy, expected_generation, &track), DISK_ERR_UNDERRUN);
  ASSERT(drive.write_gate_assertions > 0);
  ASSERT_EQ(drive.write_gate_assertions, drive.write_gate_deassertions);
  ASSERT(!drive.write_gate_active);
  ASSERT_EQ(floppy.stats.underruns, 3);
}

TEST(test_dma_stall_before_gate_is_timeout) {
  setup_floppy();
  drive.tx_stall = true;
  track_t track;
  sim_fill_track(&track, 0, 0, 0);
  ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &track),
            DISK_ERR_TIMEOUT);
  ASSERT_EQ(drive.write_gate_assertions, 0);
  ASSERT_EQ(drive.write_capture_count, 0);
}

TEST(test_write_exceeding_flux_timeout_closes_gate) {
  setup_floppy();
  track_t track;
  sim_fill_track(&track, 0, 0, 0);
  ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &track), DISK_ERR_TIMEOUT);
  ASSERT_EQ(drive.write_gate_assertions, 3);
  ASSERT_EQ(drive.write_gate_deassertions, 3);
  ASSERT(!drive.write_gate_active);
  ASSERT(drive.write_capture_count > 0);
  ASSERT_EQ(floppy.stats.underruns, 0);
  ASSERT_EQ(floppy.stats.timeout, 1);
}

int main(void) {
  pio_sim_init(&drive);
  printf("=== Floppy TX Starvation Tests ===\n\n");
  RUN_TEST(test_tx_starvation_after_gate_is_explicit_underrun);
  RUN_TEST(test_dma_stall_before_gate_is_timeout);
  RUN_TEST(test_write_exceeding_flux_timeout_closes_gate);
  ASSERT_EQ(floppy_deinit(&floppy), DISK_OK);
  pio_sim_free(&drive);
  TEST_RESULTS();
}
