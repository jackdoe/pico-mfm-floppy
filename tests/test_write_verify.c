#include "test.h"
#include "pio_sim.h"
#include "../src/floppy.h"
#include "../src/mfm_encode.h"

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

static void fill_track(track_t *track, uint8_t cylinder, uint8_t head, uint8_t salt) {
  memset(track, 0, sizeof(*track));
  track->cylinder = cylinder;
  track->head = head;
  track->valid = DISK_TRACK_VALID;
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    for (size_t byte = 0; byte < DISK_SECTOR_SIZE; byte++) {
      track->data[sector][byte] =
          (uint8_t)((size_t)salt + (size_t)sector * 19u + byte * 7u);
    }
  }
}

static uint32_t install_track(const track_t *track) {
  static uint8_t pulses[200000];
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_track(&encoder, track);
  ASSERT(!encoder.stopped);
  pio_sim_track_t *sim_track = &drive.tracks[track->cylinder][track->head];
  sim_track->deltas = malloc(encoder.pos * sizeof(*sim_track->deltas));
  ASSERT(sim_track->deltas != NULL);
  ASSERT(encoder.pos <= UINT32_MAX);
  sim_track->count = (uint32_t)encoder.pos;
  uint64_t cycles = 0;
  for (size_t i = 0; i < encoder.pos; i++) {
    sim_track->deltas[i] = pulses[i] + MFM_PIO_OVERHEAD;
    cycles += sim_track->deltas[i];
  }
  ASSERT(cycles < UINT32_MAX - 48000u);
  return (uint32_t)cycles;
}

static void install_alternate(const track_t *track) {
  uint8_t *pulses = malloc(PIO_SIM_MAX_FLUX);
  ASSERT(pulses != NULL);
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, PIO_SIM_MAX_FLUX);
  mfm_encode_track(&encoder, track);
  ASSERT(!encoder.stopped);
  ASSERT(encoder.pos <= UINT32_MAX);
  drive.alternate_track.deltas =
      malloc(encoder.pos * sizeof(*drive.alternate_track.deltas));
  ASSERT(drive.alternate_track.deltas != NULL);
  drive.alternate_track.count = (uint32_t)encoder.pos;
  for (size_t i = 0; i < encoder.pos; i++) {
    drive.alternate_track.deltas[i] = pulses[i] + MFM_PIO_OVERHEAD;
  }
  free(pulses);
}

static void setup_floppy(uint8_t old_salt) {
  floppy_deinit(&floppy);
  pio_sim_free(&drive);
  pio_sim_init(&drive);
  pio_sim_install(&drive);
  track_t old_track;
  fill_track(&old_track, 0, 0, old_salt);
  uint32_t cycles = install_track(&old_track);
  drive.write_revolution_override = cycles + 48000u;
  memset(&floppy, 0, sizeof(floppy));
  pio_sim_floppy_ref = &floppy;
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_OK);
}

TEST(test_write_is_index_to_index_finite_dma_and_verifies_twice) {
  setup_floppy(0x10);
  track_t expected;
  fill_track(&expected, 0, 0, 0x91);
  uint32_t before_samples = drive.flux_sample_reads;
  ASSERT_EQ(floppy_write_track(&floppy, generation(), &expected),
            BLOCK_OK);
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
  ASSERT_EQ(floppy_read_track(&floppy, generation(), &actual),
            BLOCK_OK);
  ASSERT_EQ(actual.valid, DISK_TRACK_VALID);
  ASSERT_MEM_EQ(actual.data, expected.data, sizeof(expected.data));
}

TEST(test_transient_physical_write_failure_retries_cleanly) {
  setup_floppy(0x20);
  drive.fault_writes_remaining = 1;
  track_t expected;
  fill_track(&expected, 0, 0, 0xA2);
  ASSERT_EQ(floppy_write_track(&floppy, generation(), &expected),
            BLOCK_OK);
  ASSERT_EQ(drive.fault_writes_remaining, 0);
  ASSERT_EQ(drive.torn_writes, 1);
  ASSERT(drive.write_gate_assertions >= 2);
  track_t actual = {.cylinder = 0, .head = 0};
  ASSERT_EQ(floppy_read_track(&floppy, generation(), &actual),
            BLOCK_OK);
  ASSERT_MEM_EQ(actual.data, expected.data, sizeof(expected.data));
}

TEST(test_permanent_physical_write_failure_is_verify_error) {
  setup_floppy(0x30);
  drive.fault_writes_remaining = 100;
  track_t expected;
  fill_track(&expected, 0, 0, 0xB3);
  ASSERT_EQ(floppy_write_track(&floppy, generation(), &expected),
            BLOCK_ERR_VERIFY);
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
    fill_track(&expected, 0, 0, 0x70 + boundary);
    ASSERT_EQ(floppy_write_track(&floppy, generation(), &expected),
              BLOCK_OK);
    ASSERT_EQ(drive.torn_writes, 1);
    track_t actual = {.cylinder = 0, .head = 0};
    ASSERT_EQ(floppy_read_track(&floppy, generation(), &actual),
              BLOCK_OK);
    ASSERT_MEM_EQ(actual.data, expected.data, sizeof(expected.data));
  }
}

TEST(test_media_swap_before_write_never_opens_gate) {
  setup_floppy(0x40);
  track_t expected;
  fill_track(&expected, 0, 0, 0xC4);
  uint32_t expected_generation = generation();
  drive.disk_changed = true;
  ASSERT_EQ(floppy_write_track(&floppy, expected_generation, &expected),
            BLOCK_ERR_MEDIA_CHANGED);
  ASSERT_EQ(drive.write_gate_assertions, 0);
  ASSERT_EQ(drive.write_capture_count, 0);
}

TEST(test_write_protection_and_partial_tracks_fail_closed) {
  setup_floppy(0x50);
  track_t expected;
  fill_track(&expected, 0, 0, 0xD5);
  drive.write_protected = true;
  ASSERT_EQ(floppy_write_track(&floppy, generation(), &expected),
            BLOCK_ERR_WRITE_PROTECTED);
  ASSERT_EQ(drive.write_gate_assertions, 0);
  drive.write_protected = false;
  expected.valid &= ~1u;
  ASSERT_EQ(floppy_write_track(&floppy, generation(), &expected),
            BLOCK_ERR_INVALID);
  ASSERT_EQ(drive.write_gate_assertions, 0);
}

TEST(test_write_protection_during_gated_tx_closes_gate_immediately) {
  setup_floppy(0x60);
  drive.write_protect_after_pulses = 3000;
  track_t expected;
  fill_track(&expected, 0, 0, 0xE6);
  ASSERT_EQ(floppy_write_track(&floppy, generation(), &expected),
            BLOCK_ERR_WRITE_PROTECTED);
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
  fill_track(&expected, 0, 0, 0xE9);
  ASSERT_EQ(floppy_write_track(&floppy, generation(), &expected),
            BLOCK_ERR_MEDIA_CHANGED);
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
  fill_track(&expected, 0, 0, 0xEA);
  ASSERT_EQ(floppy_write_track(&floppy, generation(), &expected), BLOCK_OK);
  ASSERT_EQ(floppy.stats.underruns, 1);
  ASSERT(drive.write_gate_assertions >= 2);
  track_t actual = {.cylinder = 0, .head = 0};
  ASSERT_EQ(floppy_read_track(&floppy, generation(), &actual), BLOCK_OK);
  ASSERT_MEM_EQ(actual.data, expected.data, sizeof(expected.data));
}

TEST(test_crc_valid_verify_mismatch_poison_is_sticky) {
  setup_floppy(0x65);
  track_t contradictory;
  fill_track(&contradictory, 0, 0, 0x11);
  install_alternate(&contradictory);
  drive.alternate_read = true;
  track_t expected;
  fill_track(&expected, 0, 0, 0xEB);
  ASSERT_EQ(floppy_write_track(&floppy, generation(), &expected), BLOCK_ERR_VERIFY);
  ASSERT_EQ(drive.write_gate_assertions, 3);
}

TEST(test_verify_rejects_matching_sector_with_stale_duplicate) {
  setup_floppy(0x61);
  drive.inject_stale_duplicate = true;
  track_t expected;
  fill_track(&expected, 0, 0, 0xF7);
  ASSERT_EQ(floppy_write_track(&floppy, generation(), &expected), BLOCK_ERR_VERIFY);
  ASSERT_EQ(drive.write_gate_assertions, 3);
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
  ASSERT_EQ(floppy_deinit(&floppy), BLOCK_OK);
  pio_sim_free(&drive);
  TEST_RESULTS();
}
