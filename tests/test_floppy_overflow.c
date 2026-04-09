#include "test.h"
#include "pio_sim.h"
#include "../src/floppy.h"

floppy_t *pio_sim_floppy_ref;

static pio_sim_drive_t sim_drive;
static floppy_t floppy;

static void setup_floppy(void) {
  memset(&sim_drive, 0, sizeof(sim_drive));
  pio_sim_init(&sim_drive);
  pio_sim_install(&sim_drive);

  memset(&floppy, 0, sizeof(floppy));
  floppy.pins.index = 1;
  floppy.pins.track0 = 2;
  floppy.pins.write_protect = 3;
  floppy.pins.read_data = 4;
  floppy.pins.disk_change = 5;
  floppy.pins.drive_select = 6;
  floppy.pins.motor_enable = 7;
  floppy.pins.direction = 8;
  floppy.pins.step = 9;
  floppy.pins.write_data = 10;
  floppy.pins.write_gate = 11;
  floppy.pins.side_select = 12;
  floppy.pins.density = 13;

  pio_sim_floppy_ref = &floppy;
  floppy_init(&floppy);
}

TEST(test_write_track_aborts_on_encode_overflow) {
  setup_floppy();

  track_t track;
  memset(&track, 0, sizeof(track));
  track.track = 0;
  track.side = 0;

  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    sector_t *sector = &track.sectors[i];
    sector->track = 0;
    sector->side = 0;
    sector->sector_n = i + 1;
    sector->size_code = 2;
    sector->valid = true;
    memset(sector->data, i, sizeof(sector->data));
  }

  ASSERT_EQ(floppy_write_track(&floppy, &track), FLOPPY_ERR_ENCODE_OVERFLOW);
  ASSERT_EQ(sim_drive.write_capture_count, 0);

  pio_sim_free(&sim_drive);
}

int main(void) {
  printf("=== Floppy Overflow Tests ===\n\n");

  RUN_TEST(test_write_track_aborts_on_encode_overflow);

  TEST_RESULTS();
}
