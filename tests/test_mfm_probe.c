#include "sim_floppy.h"
#include "mfm_probe.h"
#include "pico/stdlib.h"
#include "pico/time.h"

static pio_sim_drive_t drive;
static floppy_t floppy;
static mfm_probe_t probe;

static void setup_track(track_t *track, uint8_t cylinder, uint8_t head) {
  sim_setup(&drive, &floppy);
  sim_fill_track(track, cylinder, head, 0x51);
  drive.write_revolution_override = sim_install_track(&drive, track) + 48000u;
}

static disk_err_t capture(const track_t *expected) {
  return mfm_probe_track(&floppy, sim_generation(&floppy), expected->cylinder,
                         expected->head, expected, &probe);
}

TEST(test_all_patterns_survive_driver_write_seek_and_motor_restart) {
  static const uint8_t cylinders[] = {0, 39, 40, 79};
  for (size_t c = 0; c < sizeof(cylinders); c++) {
    for (uint8_t head = 0; head < DISK_HEADS; head++) {
      for (unsigned pattern = 0; pattern < MFM_TEST_PATTERNS; pattern++) {
        track_t expected;
        setup_track(&expected, cylinders[c], head);
        uint32_t generation = sim_generation(&floppy);
        ASSERT(mfm_test_fill(&expected, pattern, 0));
        ASSERT_EQ(
            floppy_write_track(&floppy, generation, &expected),
            DISK_OK);
        ASSERT_EQ(floppy_seek(&floppy, cylinders[c] == 0 ? 79 : 0), DISK_OK);
        ASSERT_EQ(floppy_motor_off(&floppy), DISK_OK);
        sleep_ms(1000);
        ASSERT_EQ(mfm_probe_track(&floppy, generation, expected.cylinder,
                                  expected.head, &expected, &probe), DISK_OK);
        ASSERT_EQ(probe.stage, MFM_PROBE_COMPLETE);
        ASSERT(mfm_probe_clean(&probe));
        ASSERT_EQ(probe.decoder.sectors_read,
                  DISK_SECTORS_PER_TRACK * MFM_PROBE_REVOLUTIONS);
        ASSERT_EQ(floppy.operation, FLOPPY_OPERATION_IDLE);
        ASSERT(!floppy.flux_active);
        ASSERT_EQ(floppy.stats.retries, 0);
        ASSERT_EQ(floppy.stats.overruns, 0);
        ASSERT_EQ(floppy.stats.underruns, 0);
      }
    }
  }
}

TEST(test_media_change_while_motor_is_off_rejects_original_generation) {
  for (unsigned stuck = 0; stuck < 2; stuck++) {
    track_t expected;
    setup_track(&expected, 0, 0);
    ASSERT_EQ(floppy_seek(&floppy, 79), DISK_OK);
    uint32_t generation = sim_generation(&floppy);
    ASSERT_EQ(floppy_motor_off(&floppy), DISK_OK);
    drive.disk_changed = true;
    drive.disk_change_stuck = stuck != 0;
    sleep_ms(1000);
    ASSERT_EQ(mfm_probe_track(&floppy, generation, 0, 0, &expected, &probe),
              DISK_ERR_MEDIA_CHANGED);
    ASSERT_EQ(probe.stage, stuck ? MFM_PROBE_SEEK : MFM_PROBE_FLUX_BEGIN);
    ASSERT_EQ(probe.revolutions, 0);
    ASSERT_EQ(drive.flux_sample_reads, 0);
    ASSERT_EQ(drive.write_gate_assertions, 0);
    ASSERT_EQ(floppy.stats.media_changes, 1);
    ASSERT(sim_generation(&floppy) != generation);
    ASSERT_EQ(floppy.operation, FLOPPY_OPERATION_IDLE);
    ASSERT(!floppy.flux_active);
  }
}

TEST(test_wrong_cylinder_and_head_are_not_counted_as_valid_sectors) {
  for (unsigned variant = 0; variant < 2; variant++) {
    track_t expected;
    setup_track(&expected, 0, 0);
    track_t wrong = expected;
    if (variant == 0)
      wrong.cylinder = 1;
    else
      wrong.head = 1;
    uint8_t pulses[PIO_SIM_MAX_FLUX];
    size_t count = sim_track_pulses(&wrong, pulses, sizeof(pulses));
    sim_install_pulses(&drive, 0, 0, pulses, count);
    ASSERT_EQ(capture(&expected), DISK_OK);
    ASSERT(!mfm_probe_clean(&probe));
    ASSERT_EQ(probe.wrong_address,
              DISK_SECTORS_PER_TRACK * MFM_PROBE_REVOLUTIONS);
    ASSERT_EQ(probe.minimum_sectors, 0);
    ASSERT_EQ(probe.sectors, 0);
  }
}

static void alternate_missing_sector(const track_t *expected) {
  static uint8_t pulses[PIO_SIM_MAX_FLUX];
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_gap(&encoder, 80);
  for (uint8_t sector = 1; sector < DISK_SECTORS_PER_TRACK; sector++) {
    mfm_encode_sector(&encoder, expected->cylinder, expected->head, sector,
                      expected->data[sector]);
    mfm_encode_gap(&encoder, 54);
  }
  ASSERT(!encoder.stopped);
  sim_track_load(&drive.alternate_track, pulses, encoder.pos);
  drive.alternate_read = true;
}

TEST(test_a_good_revolution_cannot_hide_a_missing_sector_on_another) {
  track_t expected;
  setup_track(&expected, 0, 0);
  alternate_missing_sector(&expected);
  ASSERT_EQ(capture(&expected), DISK_OK);
  ASSERT_EQ(probe.revolutions, MFM_PROBE_REVOLUTIONS);
  ASSERT_EQ(probe.sectors, DISK_TRACK_VALID);
  ASSERT_EQ(probe.minimum_sectors, DISK_SECTORS_PER_TRACK - 1u);
  ASSERT(!mfm_probe_clean(&probe));
}

TEST(test_crc_valid_wrong_data_is_rejected) {
  track_t expected;
  setup_track(&expected, 40, 1);
  expected.data[8][257] ^= 1u;
  ASSERT_EQ(capture(&expected), DISK_OK);
  ASSERT_EQ(probe.decoder.crc_errors, 0);
  ASSERT_EQ(probe.mismatches, MFM_PROBE_REVOLUTIONS);
  ASSERT(!mfm_probe_clean(&probe));
}

TEST(test_duplicate_sector_is_rejected) {
  track_t expected;
  setup_track(&expected, 0, 0);
  static uint8_t pulses[PIO_SIM_MAX_FLUX];
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_track(&encoder, &expected);
  mfm_encode_sector(&encoder, 0, 0, 0, expected.data[0]);
  mfm_encode_gap(&encoder, 54);
  ASSERT(!encoder.stopped);
  sim_install_pulses(&drive, 0, 0, pulses, encoder.pos);
  ASSERT_EQ(capture(&expected), DISK_OK);
  ASSERT_EQ(probe.minimum_sectors, DISK_SECTORS_PER_TRACK);
  ASSERT_EQ(probe.duplicates, MFM_PROBE_REVOLUTIONS);
  ASSERT(!mfm_probe_clean(&probe));
}

TEST(test_stuck_index_and_overrun_fail_and_release_capture) {
  for (unsigned variant = 0; variant < 3; variant++) {
    track_t expected;
    setup_track(&expected, 0, 0);
    ASSERT_EQ(floppy_motor_on(&floppy), DISK_OK);
    uint32_t before = drive.flux_sample_reads;
    uint64_t started = pico_test_time_us;
    if (variant < 2) {
      drive.index_stuck = true;
      drive.index_value = variant != 0;
    } else {
      drive.rx_burst_words = FLOPPY_FLUX_RING_WORDS + 1u;
    }
    ASSERT_EQ(capture(&expected),
              variant < 2 ? DISK_ERR_TIMEOUT : DISK_ERR_OVERRUN);
    ASSERT(pico_test_time_us - started <= 5100000u);
    ASSERT(!mfm_probe_clean(&probe));
    ASSERT_EQ(floppy.operation, FLOPPY_OPERATION_IDLE);
    ASSERT(!floppy.flux_active);
    if (variant < 2) ASSERT(drive.flux_sample_reads > before);
    drive.index_stuck = false;
    ASSERT_EQ(floppy_seek(&floppy, 1), DISK_OK);
  }
}

TEST(test_media_change_during_capture_is_reported) {
  track_t expected;
  setup_track(&expected, 0, 0);
  drive.disk_change_after_samples = 1000;
  ASSERT_EQ(capture(&expected), DISK_ERR_MEDIA_CHANGED);
  ASSERT_EQ(probe.stage, MFM_PROBE_CAPTURE);
  ASSERT(!mfm_probe_clean(&probe));
  ASSERT(!floppy.flux_active);
  ASSERT_EQ(floppy.operation, FLOPPY_OPERATION_IDLE);
}

TEST(test_pattern_rounds_distinguish_stale_random_data) {
  track_t first = {.cylinder = 79, .head = 1};
  track_t next = first;
  ASSERT(mfm_test_fill(&first, MFM_TEST_PATTERNS - 1u, 0));
  ASSERT(mfm_test_fill(&next, MFM_TEST_PATTERNS - 1u, 1));
  for (unsigned sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    ASSERT(memcmp(first.data[sector], next.data[sector], DISK_SECTOR_SIZE) !=
           0);
  }
  ASSERT(!mfm_test_fill(&next, MFM_TEST_PATTERNS, 0));
}

int main(void) {
  pio_sim_init(&drive);
  RUN_TEST(test_all_patterns_survive_driver_write_seek_and_motor_restart);
  RUN_TEST(test_media_change_while_motor_is_off_rejects_original_generation);
  RUN_TEST(test_wrong_cylinder_and_head_are_not_counted_as_valid_sectors);
  RUN_TEST(test_a_good_revolution_cannot_hide_a_missing_sector_on_another);
  RUN_TEST(test_crc_valid_wrong_data_is_rejected);
  RUN_TEST(test_duplicate_sector_is_rejected);
  RUN_TEST(test_stuck_index_and_overrun_fail_and_release_capture);
  RUN_TEST(test_media_change_during_capture_is_reported);
  RUN_TEST(test_pattern_rounds_distinguish_stale_random_data);
  ASSERT_EQ(floppy_deinit(&floppy), DISK_OK);
  pio_sim_free(&drive);
  TEST_RESULTS();
}
