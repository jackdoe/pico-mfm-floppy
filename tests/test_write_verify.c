#include "sim_floppy.h"

static pio_sim_drive_t drive;
static floppy_t floppy;

static void install_alternate(const track_t *track) {
  static uint8_t pulses[PIO_SIM_MAX_FLUX];
  size_t count = sim_track_pulses(track, pulses, sizeof(pulses));
  sim_track_load(&drive.alternate_track, pulses, count);
}

static void setup_floppy(uint8_t old_salt) {
  sim_setup(&drive, &floppy);
  track_t old_track;
  sim_fill_track(&old_track, 0, 0, old_salt);
  drive.write_revolution_override = sim_install_track(&drive, &old_track) + 48000u;
}

TEST(test_write_is_index_to_index_finite_dma_and_verifies_twice) {
  setup_floppy(0x10);
  track_t expected;
  sim_fill_track(&expected, 0, 0, 0x91);
  uint32_t before_samples = drive.flux_sample_reads;
  ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &expected),
            DISK_OK);
  ASSERT(drive.write_started_at_index);
  ASSERT(drive.write_ended_at_index);
  ASSERT_EQ(drive.write_gate_assertions, 1);
  ASSERT_EQ(drive.write_gate_deassertions, 1);
  ASSERT(!drive.write_gate_active);
  ASSERT(drive.dma_tx_configurations > 2);
  ASSERT_EQ(drive.dma_tx_min_count, FLOPPY_TX_HALF_BYTES);
  ASSERT_EQ(drive.dma_tx_max_count, FLOPPY_TX_HALF_BYTES);
  ASSERT(!drive.dma_source_overread);
  ASSERT(drive.dma_high_priority);
  ASSERT(drive.write_capture_count > 0);
  ASSERT(drive.flux_sample_reads - before_samples >= drive.write_capture_count * 2u);

  track_t actual = {.cylinder = 0, .head = 0};
  ASSERT_EQ(floppy_read_track(&floppy, sim_generation(&floppy), &actual),
            DISK_OK);
  ASSERT_EQ(actual.valid, DISK_TRACK_VALID);
  ASSERT_MEM_EQ(actual.data, expected.data, sizeof(expected.data));
}

TEST(test_transient_physical_write_failure_retries_cleanly) {
  setup_floppy(0x20);
  drive.fault_writes_remaining = 1;
  track_t expected;
  sim_fill_track(&expected, 0, 0, 0xA2);
  ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &expected),
            DISK_OK);
  ASSERT_EQ(drive.fault_writes_remaining, 0);
  ASSERT_EQ(drive.torn_writes, 1);
  ASSERT(drive.write_gate_assertions >= 2);
  track_t actual = {.cylinder = 0, .head = 0};
  ASSERT_EQ(floppy_read_track(&floppy, sim_generation(&floppy), &actual),
            DISK_OK);
  ASSERT_MEM_EQ(actual.data, expected.data, sizeof(expected.data));
}

TEST(test_permanent_physical_write_failure_is_verify_error) {
  setup_floppy(0x30);
  drive.fault_writes_remaining = 100;
  track_t expected;
  sim_fill_track(&expected, 0, 0, 0xB3);
  ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &expected),
            DISK_ERR_VERIFY);
  ASSERT_EQ(drive.write_gate_assertions, 3);
  ASSERT_EQ(drive.write_gate_assertions, drive.write_gate_deassertions);
  ASSERT_EQ(drive.torn_writes, 3);
}

TEST(test_retry_replaces_tears_at_every_sector_boundary) {
  for (uint8_t boundary = 1; boundary < DISK_SECTORS_PER_TRACK; boundary++) {
    setup_floppy(0x31 + boundary);
    uint32_t pulses = drive.tracks[0][0].count;
    drive.tear_after_pulses =
        (uint32_t)(((uint64_t)pulses * boundary) / DISK_SECTORS_PER_TRACK);
    drive.fault_writes_remaining = 1;
    track_t expected;
    sim_fill_track(&expected, 0, 0, 0x70 + boundary);
    ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &expected),
              DISK_OK);
    ASSERT_EQ(drive.torn_writes, 1);
    track_t actual = {.cylinder = 0, .head = 0};
    ASSERT_EQ(floppy_read_track(&floppy, sim_generation(&floppy), &actual),
              DISK_OK);
    ASSERT_MEM_EQ(actual.data, expected.data, sizeof(expected.data));
  }
}

TEST(test_media_swap_before_write_never_opens_gate) {
  setup_floppy(0x40);
  track_t expected;
  sim_fill_track(&expected, 0, 0, 0xC4);
  ASSERT_EQ(floppy_select(&floppy, true), DISK_OK);
  uint32_t expected_generation = sim_generation(&floppy);
  drive.disk_changed = true;
  ASSERT_EQ(floppy_write_track(&floppy, expected_generation, &expected),
            DISK_ERR_MEDIA_CHANGED);
  ASSERT_EQ(drive.write_gate_assertions, 0);
  ASSERT_EQ(drive.write_capture_count, 0);
}

TEST(test_write_protection_and_partial_tracks_fail_closed) {
  setup_floppy(0x50);
  track_t expected;
  sim_fill_track(&expected, 0, 0, 0xD5);
  drive.write_protected = true;
  ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &expected),
            DISK_ERR_WRITE_PROTECTED);
  ASSERT_EQ(drive.write_gate_assertions, 0);
  drive.write_protected = false;
  expected.valid &= ~1u;
  ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &expected),
            DISK_ERR_INVALID);
  ASSERT_EQ(drive.write_gate_assertions, 0);
}

TEST(test_write_protection_during_gated_tx_closes_gate_immediately) {
  setup_floppy(0x60);
  drive.write_protect_after_pulses = 3000;
  track_t expected;
  sim_fill_track(&expected, 0, 0, 0xE6);
  ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &expected),
            DISK_ERR_WRITE_PROTECTED);
  ASSERT_EQ(drive.write_gate_assertions, 1);
  ASSERT_EQ(drive.write_gate_deassertions, 1);
  ASSERT(!drive.write_gate_active);
  ASSERT(drive.write_capture_count >= drive.write_protect_after_pulses);
  ASSERT(drive.write_capture_count < 10000u);
}

TEST(test_media_change_during_gated_tx_closes_gate_immediately) {
  setup_floppy(0x63);
  drive.disk_change_after_pulses = 3000;
  track_t expected;
  sim_fill_track(&expected, 0, 0, 0xE9);
  ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &expected),
            DISK_ERR_MEDIA_CHANGED);
  ASSERT_EQ(drive.write_gate_assertions, 1);
  ASSERT_EQ(drive.write_gate_deassertions, 1);
  ASSERT(!drive.write_gate_active);
  ASSERT(drive.write_capture_count >= drive.disk_change_after_pulses);
}

TEST(test_transient_pio_tx_stall_rewrites_whole_track) {
  setup_floppy(0x64);
  drive.tx_transient_stall = true;
  drive.tx_transient_stall_after_pulses = 1000;
  track_t expected;
  sim_fill_track(&expected, 0, 0, 0xEA);
  ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &expected), DISK_OK);
  ASSERT_EQ(floppy.stats.underruns, 1);
  ASSERT(drive.write_gate_assertions >= 2);
  track_t actual = {.cylinder = 0, .head = 0};
  ASSERT_EQ(floppy_read_track(&floppy, sim_generation(&floppy), &actual), DISK_OK);
  ASSERT_MEM_EQ(actual.data, expected.data, sizeof(expected.data));
}

TEST(test_crc_valid_verify_mismatch_poison_is_sticky) {
  setup_floppy(0x65);
  track_t contradictory;
  sim_fill_track(&contradictory, 0, 0, 0x11);
  install_alternate(&contradictory);
  drive.alternate_read = true;
  track_t expected;
  sim_fill_track(&expected, 0, 0, 0xEB);
  ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &expected), DISK_ERR_VERIFY);
  ASSERT_EQ(drive.write_gate_assertions, 3);
}

TEST(test_verify_rejects_matching_sector_with_stale_duplicate) {
  setup_floppy(0x61);
  drive.inject_stale_duplicate = true;
  track_t expected;
  sim_fill_track(&expected, 0, 0, 0xF7);
  ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &expected), DISK_ERR_VERIFY);
  ASSERT_EQ(drive.write_gate_assertions, 3);
}

TEST(test_media_change_during_verify_read_fails_closed) {
  setup_floppy(0x66);
  track_t expected;
  sim_fill_track(&expected, 0, 0, 0xEC);
  drive.disk_change_after_samples = drive.flux_sample_reads + 100u;
  ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &expected),
            DISK_ERR_MEDIA_CHANGED);
  ASSERT_EQ(drive.write_gate_assertions, 1);
  ASSERT_EQ(drive.write_gate_deassertions, 1);
  ASSERT(!drive.write_gate_active);
  ASSERT(drive.write_capture_count > 0);
  ASSERT_EQ(floppy.stats.media_changes, 1);
}

TEST(test_media_change_or_write_protect_before_gate_never_opens_it) {
  for (int variant = 0; variant < 4; variant++) {
    setup_floppy((uint8_t)(0x67 + variant));
    bool protect = (variant & 1) != 0;
    drive.tx_stall = (variant & 2) != 0;
    drive.write_protect_on_tx_configure = protect;
    drive.disk_change_on_tx_configure = !protect;
    track_t expected;
    sim_fill_track(&expected, 0, 0, (uint8_t)(0xF0 + variant));
    ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &expected),
              protect ? DISK_ERR_WRITE_PROTECTED : DISK_ERR_MEDIA_CHANGED);
    ASSERT_EQ(drive.dma_tx_configurations, 1);
    ASSERT_EQ(drive.write_gate_assertions, 0);
    ASSERT_EQ(drive.write_capture_count, 0);
  }
}

TEST(test_track_longer_than_revolution_is_verify_error) {
  setup_floppy(0x6B);
  drive.write_revolution_override /= 2u;
  track_t expected;
  sim_fill_track(&expected, 0, 0, 0xF4);
  ASSERT_EQ(floppy_write_track(&floppy, sim_generation(&floppy), &expected), DISK_ERR_VERIFY);
  ASSERT_EQ(drive.write_gate_assertions, 3);
  ASSERT_EQ(drive.write_gate_deassertions, 3);
  ASSERT(!drive.write_gate_active);
  ASSERT(drive.write_ended_at_index);
  ASSERT_EQ(floppy.stats.retries, 3);
}

int main(void) {
  pio_sim_init(&drive);
  printf("=== Physical Track Write Verification Tests ===\n\n");
  RUN_TEST(test_write_is_index_to_index_finite_dma_and_verifies_twice);
  RUN_TEST(test_transient_physical_write_failure_retries_cleanly);
  RUN_TEST(test_permanent_physical_write_failure_is_verify_error);
  RUN_TEST(test_retry_replaces_tears_at_every_sector_boundary);
  RUN_TEST(test_media_swap_before_write_never_opens_gate);
  RUN_TEST(test_write_protection_and_partial_tracks_fail_closed);
  RUN_TEST(test_write_protection_during_gated_tx_closes_gate_immediately);
  RUN_TEST(test_media_change_during_gated_tx_closes_gate_immediately);
  RUN_TEST(test_transient_pio_tx_stall_rewrites_whole_track);
  RUN_TEST(test_crc_valid_verify_mismatch_poison_is_sticky);
  RUN_TEST(test_verify_rejects_matching_sector_with_stale_duplicate);
  RUN_TEST(test_media_change_during_verify_read_fails_closed);
  RUN_TEST(test_media_change_or_write_protect_before_gate_never_opens_it);
  RUN_TEST(test_track_longer_than_revolution_is_verify_error);
  ASSERT_EQ(floppy_deinit(&floppy), DISK_OK);
  pio_sim_free(&drive);
  TEST_RESULTS();
}
