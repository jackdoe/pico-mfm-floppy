#include "test.h"
#include "pio_sim.h"
#include "../src/floppy.h"

floppy_t *pio_sim_floppy_ref;

static pio_sim_drive_t drive;
static floppy_t floppy;

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
  drive.write_revolution_override = 4800000u;
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
    memset(track->data[sector], sector * 13u, DISK_SECTOR_SIZE);
  }
}

TEST(test_tx_starvation_after_gate_is_explicit_underrun) {
  setup_floppy();
  drive.tx_force_underrun = true;
  drive.tx_force_underrun_repeat = true;
  track_t track;
  fill_track(&track);
  uint32_t expected_generation = generation();
  ASSERT_EQ(floppy_write_track(&floppy, expected_generation, &track), BLOCK_ERR_UNDERRUN);
  ASSERT(drive.write_gate_assertions > 0);
  ASSERT_EQ(drive.write_gate_assertions, drive.write_gate_deassertions);
  ASSERT(!drive.write_gate_active);
  ASSERT_EQ(floppy.stats.underruns, 3);
}

TEST(test_dma_stall_before_gate_is_timeout) {
  setup_floppy();
  drive.tx_stall = true;
  track_t track;
  fill_track(&track);
  ASSERT_EQ(floppy_write_track(&floppy, generation(), &track),
            BLOCK_ERR_TIMEOUT);
  ASSERT_EQ(drive.write_gate_assertions, 0);
  ASSERT_EQ(drive.write_capture_count, 0);
}

int main(void) {
  pio_sim_init(&drive);
  printf("=== Floppy TX Starvation Tests ===\n\n");
  RUN_TEST(test_tx_starvation_after_gate_is_explicit_underrun);
  RUN_TEST(test_dma_stall_before_gate_is_timeout);
  ASSERT_EQ(floppy_deinit(&floppy), BLOCK_OK);
  pio_sim_free(&drive);
  TEST_RESULTS();
}
