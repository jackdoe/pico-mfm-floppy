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

static void fill_track(track_t *track) {
  memset(track, 0, sizeof(*track));
  track->track = 0;
  track->side = 0;

  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    sector_t *sector = &track->sectors[i];
    sector->track = 0;
    sector->side = 0;
    sector->sector_n = i + 1;
    sector->size_code = 2;
    sector->valid = true;
    memset(sector->data, i, sizeof(sector->data));
  }
}

TEST(test_read_sector_times_out_without_flux) {
  setup_floppy();

  sector_t sector = {
    .track = 0,
    .side = 0,
    .sector_n = 1,
    .size_code = 2,
    .valid = false,
  };

  ASSERT_EQ(floppy_read_sector(&floppy, &sector), FLOPPY_ERR_TIMEOUT);
  ASSERT(!sector.valid);

  pio_sim_free(&sim_drive);
}

TEST(test_write_track_times_out_without_index) {
  setup_floppy();
  sim_drive.index_stuck = true;
  sim_drive.index_value = false;

  track_t track;
  fill_track(&track);

  ASSERT_EQ(floppy_write_track(&floppy, &track), FLOPPY_ERR_TIMEOUT);
  ASSERT_EQ(sim_drive.write_capture_count, 0);

  pio_sim_free(&sim_drive);
}

int main(void) {
  printf("=== Floppy Timeout Tests ===\n\n");

  RUN_TEST(test_read_sector_times_out_without_flux);
  RUN_TEST(test_write_track_times_out_without_index);

  TEST_RESULTS();
}
