#include "test.h"
#include "pio_sim.h"
#include "hardware/gpio.h"
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

static void write_le32(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8u);
  bytes[2] = (uint8_t)(value >> 16u);
  bytes[3] = (uint8_t)(value >> 24u);
}

static void scp_checksum(uint8_t *image, size_t size) {
  uint32_t checksum = 0;
  for (size_t i = 0x10u; i < size; i++) checksum += image[i];
  write_le32(image + 0x0Cu, checksum);
}

static size_t make_minimal_scp(uint8_t *image, size_t capacity) {
  size_t required = 0x10u + 168u * 4u + DISK_TRACK_COUNT * 18u;
  ASSERT(capacity >= required);
  memset(image, 0, required);
  image[0] = 'S';
  image[1] = 'C';
  image[2] = 'P';
  image[5] = 1;
  image[7] = DISK_TRACK_COUNT - 1u;
  image[8] = 1;
  size_t position = 0x10u + 168u * 4u;
  for (uint16_t track = 0; track < DISK_TRACK_COUNT; track++) {
    ASSERT(position <= UINT32_MAX);
    write_le32(image + 0x10u + (size_t)track * 4u, (uint32_t)position);
    image[position] = 'T';
    image[position + 1u] = 'R';
    image[position + 2u] = 'K';
    image[position + 3u] = (uint8_t)track;
    write_le32(image + position + 4u, 100u);
    write_le32(image + position + 8u, 1u);
    write_le32(image + position + 12u, 16u);
    image[position + 16u] = 0;
    image[position + 17u] = 100;
    position += 18u;
  }
  scp_checksum(image, required);
  return required;
}

static void fill_track(track_t *track, uint8_t cylinder, uint8_t head, uint8_t salt) {
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

static void install_pulses(uint8_t cylinder, uint8_t head, const uint8_t *pulses,
                           size_t count) {
  pio_sim_track_t *track = &drive.tracks[cylinder][head];
  free(track->deltas);
  track->deltas = malloc(count * sizeof(*track->deltas));
  ASSERT(track->deltas != NULL);
  ASSERT(count <= UINT32_MAX);
  track->count = (uint32_t)count;
  for (size_t i = 0; i < count; i++) track->deltas[i] = pulses[i] + MFM_PIO_OVERHEAD;
}

static void install_track(uint8_t physical_cylinder, uint8_t physical_head,
                          uint8_t claimed_cylinder, uint8_t claimed_head,
                          uint8_t salt) {
  static uint8_t pulses[200000];
  track_t track;
  fill_track(&track, claimed_cylinder, claimed_head, salt);
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_track(&encoder, &track);
  ASSERT(!encoder.stopped);
  install_pulses(physical_cylinder, physical_head, pulses, encoder.pos);
}

static void setup_floppy(void) {
  floppy_deinit(&floppy);
  memset(&floppy, 0, sizeof(floppy));
  drive.head_track = 0;
  drive.head_side = 0;
  drive.index_stuck = false;
  drive.disk_changed = false;
  drive.write_protected = false;
  drive.alternate_read = false;
  drive.index_poll_count = 0;
  drive.index_period_us = 200000u;
  drive.index_low_us = 2000u;
  drive.write_protect_after_pulses = 0;
  drive.disk_change_after_pulses = 0;
  drive.disk_change_after_samples = 0;
  drive.tx_transient_stall = false;
  drive.rx_pio_stall = false;
  drive.rx_pio_stall_repeat = false;
  drive.pio_gpio_base = 0;
  ASSERT(pio_sim_set_noise(&drive, (flux_noise_config_t){0}));
  pio_sim_floppy_ref = &floppy;
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_OK);
}

TEST(test_init_configures_hd_open_drain) {
  setup_floppy();
  ASSERT(drive.density_hd);
  ASSERT_EQ(floppy_init(NULL, test_pins()), BLOCK_ERR_INVALID);
}

TEST(test_public_read_entry_points_reject_null_and_uninitialized_instances) {
  uint8_t data[DISK_SECTOR_SIZE];
  track_t track = {.cylinder = 0, .head = 0};
  floppy_t empty = {0};
  floppy_t automatic;
  ASSERT_EQ(floppy_read_sector(NULL, 0, 0, 0, 0, data), BLOCK_ERR_INVALID);
  ASSERT_EQ(floppy_read_sector(&empty, 0, 0, 0, 0, data), BLOCK_ERR_INVALID);
  ASSERT_EQ(floppy_read_sector(&automatic, 0, 0, 0, 0, data),
            BLOCK_ERR_INVALID);
  ASSERT_EQ(floppy_read_track(NULL, 0, &track), BLOCK_ERR_INVALID);
  ASSERT_EQ(floppy_read_track(&empty, 0, &track), BLOCK_ERR_INVALID);
  block_device_t device = floppy_device(NULL);
  ASSERT_EQ(device.read_track(device.ctx, 0, 0, 0, &track),
            BLOCK_ERR_INVALID);
}

TEST(test_uninitialized_automatic_context_initializes) {
  ASSERT_EQ(floppy_deinit(&floppy), BLOCK_OK);
  floppy_t automatic;
  pio_sim_floppy_ref = &automatic;
  ASSERT_EQ(floppy_init(&automatic, test_pins()), BLOCK_OK);
  ASSERT_EQ(floppy_init(&automatic, test_pins()), BLOCK_ERR_BUSY);
  ASSERT_EQ(floppy_deinit(&automatic), BLOCK_OK);
  pio_sim_floppy_ref = &floppy;
}

TEST(test_scp_loader_rejects_checksum_and_nonstandard_width) {
  uint8_t image[0x10u + 168u * 4u + DISK_TRACK_COUNT * 18u];
  size_t image_size = make_minimal_scp(image, sizeof(image));
  pio_sim_drive_t isolated;
  pio_sim_init(&isolated);
  ASSERT(pio_sim_load_scp(&isolated, image, image_size));
  image[image_size - 1u] ^= 1u;
  ASSERT(!pio_sim_load_scp(&isolated, image, image_size));
  image[image_size - 1u] ^= 1u;
  scp_checksum(image, image_size);
  image[9] = 1;
  ASSERT(!pio_sim_load_scp(&isolated, image, image_size));
  image[9] = 0;
  image[image_size - 1u] = 0;
  scp_checksum(image, image_size);
  ASSERT(!pio_sim_load_scp(&isolated, image, image_size));
  image[image_size - 1u] = 100;
  write_le32(image + 0x10u, 1u);
  scp_checksum(image, image_size);
  ASSERT(!pio_sim_load_scp(&isolated, image, image_size));
  pio_sim_free(&isolated);
  pio_sim_install(&drive);
}

static void assert_no_resources(void) {
  ASSERT_EQ(drive.programs_loaded, 0);
  ASSERT_EQ(drive.sms_claimed, 0);
  ASSERT_EQ(drive.dma_channels_claimed, 0);
  ASSERT_EQ(drive.spin_locks_claimed, 0);
  ASSERT_EQ(drive.timers_active, 0);
}

TEST(test_distinct_hardware_owners_are_exclusive_and_nonmutating) {
  setup_floppy();
  floppy_t other;
  uint8_t before[sizeof(other)];
  memset(&other, 0xA5, sizeof(other));
  memcpy(before, &other, sizeof(other));

  ASSERT_EQ(floppy_init(&other, test_pins()), BLOCK_ERR_BUSY);
  ASSERT_MEM_EQ(&other, before, sizeof(other));
  ASSERT_EQ(floppy_deinit(&other), BLOCK_ERR_INVALID);
  ASSERT_MEM_EQ(&other, before, sizeof(other));
  floppy_stats_t stats;
  ASSERT_EQ(floppy_stats(&floppy, &stats), BLOCK_OK);

  ASSERT_EQ(floppy_deinit(&floppy), BLOCK_OK);
  assert_no_resources();
  pio_sim_floppy_ref = &other;
  ASSERT_EQ(floppy_init(&other, test_pins()), BLOCK_OK);
  ASSERT_EQ(floppy_stats(&other, &stats), BLOCK_OK);
  ASSERT_EQ(floppy_deinit(&other), BLOCK_OK);
  assert_no_resources();
  pio_sim_floppy_ref = &floppy;
}

TEST(test_init_deinit_and_resource_failures_are_leak_free) {
  setup_floppy();
  ASSERT_EQ(drive.programs_loaded, 2);
  ASSERT_EQ(drive.sms_claimed, 2);
  ASSERT_EQ(drive.dma_channels_claimed, 1);
  ASSERT_EQ(drive.spin_locks_claimed, 1);
  ASSERT_EQ(drive.timers_active, 1);
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_ERR_BUSY);
  ASSERT_EQ(drive.programs_loaded, 2);
  ASSERT_EQ(drive.spin_locks_claimed, 1);
  uint32_t before_deinit = drive.gpio_deinits;
  ASSERT_EQ(floppy_deinit(&floppy), BLOCK_OK);
  ASSERT_EQ(drive.gpio_deinits - before_deinit, 13);
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_OK);
  ASSERT_EQ(drive.programs_loaded, 2);
  ASSERT_EQ(drive.sms_claimed, 2);
  ASSERT_EQ(drive.dma_channels_claimed, 1);
  ASSERT_EQ(drive.timers_active, 1);
  ASSERT_EQ(floppy_deinit(&floppy), BLOCK_OK);
  assert_no_resources();
  ASSERT_EQ(floppy_deinit(&floppy), BLOCK_ERR_INVALID);

  drive.fail_spinlock_claim = true;
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_ERR_IO);
  assert_no_resources();
  drive.fail_spinlock_claim = false;

  drive.fail_pio_capacity = true;
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_ERR_IO);
  assert_no_resources();
  drive.fail_pio_capacity = false;

  drive.program_add_calls = 0;
  drive.fail_program_add_call = 1;
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_ERR_IO);
  assert_no_resources();

  drive.program_add_calls = 0;
  drive.fail_program_add_call = 2;
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_ERR_IO);
  assert_no_resources();
  drive.fail_program_add_call = 0;

  drive.program_add_calls = 0;
  drive.sm_claim_calls = 0;
  drive.fail_sm_claim_call = 1;
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_ERR_IO);
  assert_no_resources();

  drive.program_add_calls = 0;
  drive.sm_claim_calls = 0;
  drive.fail_sm_claim_call = 2;
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_ERR_IO);
  assert_no_resources();
  drive.fail_sm_claim_call = 0;

  drive.program_add_calls = 0;
  drive.sm_claim_calls = 0;
  drive.fail_dma_claim = true;
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_ERR_IO);
  assert_no_resources();
  drive.fail_dma_claim = false;

  drive.program_add_calls = 0;
  drive.sm_claim_calls = 0;
  drive.fail_read_program_init = true;
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_ERR_IO);
  assert_no_resources();
  drive.fail_read_program_init = false;

  drive.program_add_calls = 0;
  drive.sm_claim_calls = 0;
  drive.fail_write_program_init = true;
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_ERR_IO);
  assert_no_resources();
  drive.fail_write_program_init = false;

  drive.program_add_calls = 0;
  drive.sm_claim_calls = 0;
  drive.fail_timer_create = true;
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_ERR_IO);
  assert_no_resources();
  drive.fail_timer_create = false;

  drive.pio_gpio_base = 16;
  ASSERT_EQ(floppy_init(&floppy, test_pins()), BLOCK_ERR_INVALID);
  assert_no_resources();
  drive.pio_gpio_base = 0;

  floppy_pins_t pins = test_pins();
  pins.density = pins.side_select;
  ASSERT_EQ(floppy_init(&floppy, pins), BLOCK_ERR_INVALID);
  assert_no_resources();
  pins = test_pins();
  pins.density = NUM_BANK0_GPIOS;
  ASSERT_EQ(floppy_init(&floppy, pins), BLOCK_ERR_INVALID);
  assert_no_resources();

  _Alignas(floppy_t) uint8_t storage[sizeof(floppy_t) + _Alignof(floppy_t)];
  floppy_t *misaligned = (floppy_t *)(void *)(storage + 1u);
  ASSERT_EQ(floppy_init(misaligned, test_pins()), BLOCK_ERR_INVALID);
}

TEST(test_read_sector_uses_zero_based_index) {
  install_track(0, 0, 0, 0, 0x20);
  setup_floppy();
  uint8_t data[DISK_SECTOR_SIZE];
  ASSERT_EQ(floppy_read_sector(&floppy, generation(),
                               0, 0, 0, data), BLOCK_OK);
  ASSERT_EQ(data[0], 0x20);
  ASSERT_EQ(data[511], (uint8_t)(0x20 + 511u));
}

TEST(test_read_track_is_one_complete_revolution) {
  install_track(4, 1, 4, 1, 0x31);
  setup_floppy();
  track_t track = {.cylinder = 4, .head = 1};
  ASSERT_EQ(floppy_read_track(&floppy, generation(), &track),
            BLOCK_OK);
  ASSERT_EQ(track.valid, DISK_TRACK_VALID);
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    ASSERT_EQ(track.data[sector][0], (uint8_t)(0x31 + sector * 17u));
  }
}

TEST(test_seeded_drive_noise_reaches_the_hardware_path) {
  install_track(6, 1, 6, 1, 0x4D);
  setup_floppy();
  ASSERT(pio_sim_set_noise(&drive, (flux_noise_config_t){
      .seed = UINT32_C(0x10293847),
      .jitter_ticks = 3,
      .drift_ppm = 15000,
      .wander_step_ppm = 250,
      .wander_limit_ppm = 8000,
      .wander_period = 256,
  }));
  track_t expected;
  fill_track(&expected, 6, 1, 0x4D);
  track_t actual = {.cylinder = 6, .head = 1};
  ASSERT_EQ(floppy_read_track(&floppy, generation(), &actual), BLOCK_OK);
  ASSERT_EQ(actual.valid, DISK_TRACK_VALID);
  ASSERT_MEM_EQ(actual.data, expected.data, sizeof(actual.data));
}

TEST(test_track_read_preserves_crc_valid_partial_sectors) {
  static uint8_t pulses[200000];
  track_t source;
  fill_track(&source, 11, 0, 0x91);
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_gap(&encoder, 80);
  for (uint8_t sector = 0; sector + 1u < DISK_SECTORS_PER_TRACK; sector++) {
    mfm_encode_sector(&encoder, 11, 0, sector, source.data[sector]);
    mfm_encode_gap(&encoder, 54);
  }
  install_pulses(11, 0, pulses, encoder.pos);
  setup_floppy();
  track_t track = {.cylinder = 11, .head = 0};
  ASSERT_EQ(floppy_read_track(&floppy, generation(), &track),
            BLOCK_ERR_TIMEOUT);
  ASSERT_EQ(track.valid,
            DISK_TRACK_VALID & ~(1u << (DISK_SECTORS_PER_TRACK - 1u)));
  for (uint8_t sector = 0; sector + 1u < DISK_SECTORS_PER_TRACK; sector++) {
    ASSERT_MEM_EQ(track.data[sector], source.data[sector], DISK_SECTOR_SIZE);
  }
}

TEST(test_stray_wrong_id_does_not_abort_requested_read) {
  static uint8_t pulses[200000];
  uint8_t stray[DISK_SECTOR_SIZE];
  memset(stray, 0xE1, sizeof(stray));
  track_t correct;
  fill_track(&correct, 8, 0, 0x44);
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_gap(&encoder, 80);
  mfm_encode_sector(&encoder, 9, 0, 0, stray);
  mfm_encode_gap(&encoder, 54);
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    mfm_encode_sector(&encoder, 8, 0, sector, correct.data[sector]);
    mfm_encode_gap(&encoder, 54);
  }
  install_pulses(8, 0, pulses, encoder.pos);
  setup_floppy();
  uint8_t data[DISK_SECTOR_SIZE];
  ASSERT_EQ(floppy_read_sector(&floppy, generation(),
                               8, 0, 0, data), BLOCK_OK);
  ASSERT_EQ(data[0], 0x44);
}

TEST(test_duplicate_id_in_one_revolution_is_corrupt) {
  static uint8_t pulses[200000];
  track_t source;
  fill_track(&source, 12, 0, 0x53);
  uint8_t stale[DISK_SECTOR_SIZE];
  memset(stale, 0xEE, sizeof(stale));
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_gap(&encoder, 80);
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    mfm_encode_sector(&encoder, 12, 0, sector, source.data[sector]);
    mfm_encode_gap(&encoder, 54);
  }
  mfm_encode_sector(&encoder, 12, 0, 0, stale);
  mfm_encode_gap(&encoder, 54);
  install_pulses(12, 0, pulses, encoder.pos);
  setup_floppy();
  track_t track = {.cylinder = 12, .head = 0};
  ASSERT_EQ(floppy_read_track(&floppy, generation(), &track), BLOCK_ERR_CORRUPT);
  ASSERT((track.valid & 1u) == 0);
  uint8_t data[DISK_SECTOR_SIZE];
  ASSERT_EQ(floppy_read_sector(&floppy, generation(), 12, 0, 0, data),
            BLOCK_ERR_CORRUPT);
}

TEST(test_wrong_ch_does_not_mask_requested_partial_track) {
  static uint8_t pulses[200000];
  track_t source;
  fill_track(&source, 13, 0, 0x64);
  uint8_t stray[DISK_SECTOR_SIZE];
  memset(stray, 0xD4, sizeof(stray));
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_gap(&encoder, 80);
  mfm_encode_sector(&encoder, 14, 0, 0, stray);
  mfm_encode_gap(&encoder, 54);
  for (uint8_t sector = 0; sector + 1u < DISK_SECTORS_PER_TRACK; sector++) {
    mfm_encode_sector(&encoder, 13, 0, sector, source.data[sector]);
    mfm_encode_gap(&encoder, 54);
  }
  install_pulses(13, 0, pulses, encoder.pos);
  setup_floppy();
  track_t track = {.cylinder = 13, .head = 0};
  ASSERT_EQ(floppy_read_track(&floppy, generation(), &track), BLOCK_ERR_TIMEOUT);
  ASSERT_EQ(track.valid,
            DISK_TRACK_VALID & ~(1u << (DISK_SECTORS_PER_TRACK - 1u)));
}

TEST(test_conflicting_data_across_revolutions_is_corrupt) {
  static uint8_t base_pulses[100000];
  static uint8_t alternate_pulses[100000];
  track_t source;
  fill_track(&source, 15, 0, 0x85);
  mfm_encode_t base;
  mfm_encode_init(&base, base_pulses, sizeof(base_pulses));
  mfm_encode_gap(&base, 80);
  for (uint8_t sector = 0; sector < 9; sector++) {
    mfm_encode_sector(&base, 15, 0, sector, source.data[sector]);
    mfm_encode_gap(&base, 54);
  }
  install_pulses(15, 0, base_pulses, base.pos);

  uint8_t conflicting[DISK_SECTOR_SIZE];
  memset(conflicting, 0xCF, sizeof(conflicting));
  mfm_encode_t alternate;
  mfm_encode_init(&alternate, alternate_pulses, sizeof(alternate_pulses));
  mfm_encode_gap(&alternate, 80);
  mfm_encode_sector(&alternate, 15, 0, 0, conflicting);
  mfm_encode_gap(&alternate, 54);
  for (uint8_t sector = 9; sector < DISK_SECTORS_PER_TRACK; sector++) {
    mfm_encode_sector(&alternate, 15, 0, sector, source.data[sector]);
    mfm_encode_gap(&alternate, 54);
  }
  drive.alternate_track.deltas =
      malloc(alternate.pos * sizeof(*drive.alternate_track.deltas));
  ASSERT(drive.alternate_track.deltas != NULL);
  ASSERT(alternate.pos <= UINT32_MAX);
  drive.alternate_track.count = (uint32_t)alternate.pos;
  for (size_t i = 0; i < alternate.pos; i++) {
    drive.alternate_track.deltas[i] = alternate_pulses[i] + MFM_PIO_OVERHEAD;
  }
  setup_floppy();
  drive.alternate_read = true;
  track_t track = {.cylinder = 15, .head = 0};
  ASSERT_EQ(floppy_read_track(&floppy, generation(), &track), BLOCK_ERR_CORRUPT);
  ASSERT((track.valid & 1u) == 0);
}

TEST(test_wrong_track_is_exact_terminal_status) {
  install_track(5, 0, 7, 0, 0x50);
  setup_floppy();
  uint8_t data[DISK_SECTOR_SIZE];
  ASSERT_EQ(floppy_read_sector(&floppy, generation(),
                               5, 0, 0, data), BLOCK_ERR_WRONG_TRACK);
  ASSERT_EQ(floppy.stats.wrong_track, 1);
}

TEST(test_persistent_wrong_track_rehomes_and_recovers) {
  install_track(5, 0, 5, 0, 0x75);
  install_track(7, 0, 7, 0, 0x77);
  setup_floppy();
  floppy.cylinder = 5;
  floppy.track0_confirmed = true;
  drive.head_track = 7;
  uint8_t data[DISK_SECTOR_SIZE];
  ASSERT_EQ(floppy_read_sector(&floppy, generation(), 5, 0, 0, data), BLOCK_OK);
  ASSERT_EQ(data[0], 0x75);
  ASSERT(floppy.stats.recovered > 0);
}

TEST(test_wrong_side_is_exact_terminal_status) {
  install_track(6, 0, 6, 1, 0x60);
  setup_floppy();
  uint8_t data[DISK_SECTOR_SIZE];
  ASSERT_EQ(floppy_read_sector(&floppy, generation(),
                               6, 0, 0, data), BLOCK_ERR_WRONG_SIDE);
  ASSERT_EQ(floppy.stats.wrong_side, 1);
}

TEST(test_media_generation_is_edge_triggered) {
  setup_floppy();
  ASSERT_EQ(generation(), 0);
  ASSERT_EQ(floppy_select(&floppy, true), BLOCK_OK);
  drive.disk_changed = true;
  ASSERT_EQ(generation(), 1);
  ASSERT_EQ(generation(), 1);
  drive.disk_changed = false;
  ASSERT_EQ(generation(), 1);
  drive.disk_changed = true;
  ASSERT_EQ(generation(), 2);
  ASSERT_EQ(floppy.stats.media_changes, 2);
}

TEST(test_media_generation_skips_zero_on_wrap) {
  setup_floppy();
  floppy.media_generation = UINT32_MAX;
  floppy.disk_change_active = false;
  ASSERT_EQ(floppy_select(&floppy, true), BLOCK_OK);
  drive.disk_changed = true;
  ASSERT_EQ(generation(), 1);
}

TEST(test_media_latch_is_cleared_and_spindle_is_qualified_per_generation) {
  install_track(0, 0, 0, 0, 0x68);
  setup_floppy();
  ASSERT_EQ(floppy_select(&floppy, true), BLOCK_OK);
  drive.disk_changed = true;
  uint32_t first = generation();
  ASSERT_EQ(first, 1);
  uint8_t data[DISK_SECTOR_SIZE];
  ASSERT_EQ(floppy_read_sector(&floppy, first, 0, 0, 0, data), BLOCK_OK);
  ASSERT(!drive.disk_changed);
  ASSERT(floppy.motor_qualified);
  ASSERT_EQ(floppy.motor_generation, first);
  drive.disk_changed = true;
  uint32_t second = generation();
  ASSERT_EQ(second, 2);
  ASSERT(!floppy.motor_qualified);
  uint8_t cylinder;
  ASSERT_EQ(floppy_current_track(&floppy, &cylinder), BLOCK_ERR_NO_TRACK0);
  ASSERT_EQ(floppy_read_sector(&floppy, second, 0, 0, 0, data), BLOCK_OK);
  ASSERT_EQ(floppy.motor_generation, second);
}

TEST(test_disk_change_output_is_gated_by_drive_selection) {
  setup_floppy();
  drive.disk_changed = true;
  bool changed = true;
  ASSERT_EQ(floppy_disk_changed(&floppy, &changed), BLOCK_OK);
  ASSERT(!changed);
  ASSERT_EQ(generation(), 0);
  ASSERT_EQ(floppy_select(&floppy, true), BLOCK_OK);
  ASSERT_EQ(floppy_disk_changed(&floppy, &changed), BLOCK_OK);
  ASSERT(changed);
  ASSERT_EQ(generation(), 1);
}

TEST(test_rx_overrun_is_explicit_and_restart_has_no_stale_data) {
  install_track(2, 0, 2, 0, 0x72);
  setup_floppy();
  uint8_t data[DISK_SECTOR_SIZE];
  uint32_t expected_generation = generation();
  drive.rx_burst_words = FLOPPY_FLUX_RING_WORDS + 32u;
  drive.rx_burst_repeat = true;
  ASSERT_EQ(floppy_read_sector(&floppy, expected_generation, 2, 0, 0, data),
            BLOCK_ERR_OVERRUN);
  ASSERT(drive.dma_high_priority);
  ASSERT(floppy.stats.overruns > 0);
  drive.rx_burst_words = 0;
  drive.rx_burst_repeat = false;
  ASSERT_EQ(floppy_read_sector(&floppy, expected_generation, 2, 0, 0, data), BLOCK_OK);
  ASSERT_EQ(data[0], 0x72);
}

TEST(test_transient_rx_overrun_recovers_from_fresh_stream) {
  install_track(3, 0, 3, 0, 0x83);
  setup_floppy();
  uint8_t data[DISK_SECTOR_SIZE];
  drive.rx_burst_words = FLOPPY_FLUX_RING_WORDS + 32u;
  drive.rx_burst_repeat = false;
  ASSERT_EQ(floppy_read_sector(&floppy, generation(),
                               3, 0, 0, data), BLOCK_OK);
  ASSERT_EQ(data[0], 0x83);
  ASSERT_EQ(floppy.stats.recovered, 1);
}

TEST(test_pio_rx_stall_is_explicit_and_recoverable) {
  install_track(3, 0, 3, 0, 0x8A);
  setup_floppy();
  uint8_t data[DISK_SECTOR_SIZE];
  drive.rx_pio_stall = true;
  ASSERT_EQ(floppy_read_sector(&floppy, generation(), 3, 0, 0, data), BLOCK_OK);
  ASSERT(floppy.stats.overruns > 0);
  ASSERT(floppy.stats.recovered > 0);
  drive.rx_pio_stall = true;
  drive.rx_pio_stall_repeat = true;
  ASSERT_EQ(floppy_read_sector(&floppy, generation(), 3, 0, 0, data),
            BLOCK_ERR_OVERRUN);
}

TEST(test_media_change_during_flux_is_linearized) {
  install_track(4, 0, 4, 0, 0x9B);
  setup_floppy();
  uint32_t expected = generation();
  drive.disk_change_after_samples = drive.flux_sample_reads + 100u;
  uint8_t data[DISK_SECTOR_SIZE];
  ASSERT_EQ(floppy_read_sector(&floppy, expected, 4, 0, 0, data),
            BLOCK_ERR_MEDIA_CHANGED);
}

TEST(test_seek_rejects_invalid_cylinder_without_moving) {
  setup_floppy();
  ASSERT_EQ(floppy_seek(&floppy, 10), BLOCK_OK);
  ASSERT_EQ(floppy_seek(&floppy, DISK_CYLINDERS), BLOCK_ERR_WRONG_TRACK);
  uint8_t cylinder;
  ASSERT_EQ(floppy_current_track(&floppy, &cylinder), BLOCK_OK);
  ASSERT_EQ(cylinder, 10);
}

TEST(test_seek_applies_final_head_settle) {
  setup_floppy();
  ASSERT_EQ(floppy_seek(&floppy, 10), BLOCK_OK);
  uint64_t before = pico_test_time_us;
  ASSERT_EQ(floppy_seek(&floppy, 11), BLOCK_OK);
  ASSERT(pico_test_time_us - before >= 26000u);
}

TEST(test_motor_and_select_are_idempotent) {
  setup_floppy();
  ASSERT_EQ(floppy_motor_on(&floppy), BLOCK_OK);
  ASSERT_EQ(floppy_motor_on(&floppy), BLOCK_OK);
  ASSERT(floppy.motor_on);
  ASSERT_EQ(floppy_motor_off(&floppy), BLOCK_OK);
  ASSERT_EQ(floppy_motor_off(&floppy), BLOCK_OK);
  ASSERT(!floppy.motor_on);
  ASSERT_EQ(floppy_select(&floppy, true), BLOCK_OK);
  ASSERT_EQ(floppy_select(&floppy, true), BLOCK_OK);
  ASSERT(floppy.selected);
  ASSERT_EQ(floppy_select(&floppy, false), BLOCK_OK);
  ASSERT_EQ(floppy_select(&floppy, false), BLOCK_OK);
  ASSERT(!floppy.selected);
}

TEST(test_motor_rejects_rapid_index_noise) {
  setup_floppy();
  drive.index_period_us = 10000u;
  drive.index_low_us = 2000u;
  ASSERT_EQ(floppy_motor_on(&floppy), BLOCK_ERR_TIMEOUT);
  ASSERT(!floppy.motor_on);
}

TEST(test_raw_flux_session_is_generation_guarded) {
  install_track(0, 0, 0, 0, 0xA0);
  setup_floppy();
  uint32_t expected_generation = generation();
  ASSERT_EQ(floppy_side_select(&floppy, 0), BLOCK_OK);
  ASSERT_EQ(floppy_flux_begin(&floppy, expected_generation), BLOCK_OK);
  ASSERT_EQ(floppy_flux_begin(&floppy, expected_generation), BLOCK_ERR_BUSY);
  uint16_t delta;
  bool index;
  ASSERT_EQ(floppy_flux_next(&floppy, &delta, &index), BLOCK_OK);
  drive.disk_changed = true;
  ASSERT_EQ(floppy_flux_next(&floppy, &delta, &index), BLOCK_ERR_MEDIA_CHANGED);
  ASSERT_EQ(floppy_flux_end(&floppy), BLOCK_OK);
  ASSERT_EQ(floppy_flux_end(&floppy), BLOCK_ERR_INVALID);
  ASSERT_EQ(floppy_flux_next(&floppy, &delta, &index), BLOCK_ERR_INVALID);
}


TEST(test_raw_flux_exclusively_owns_motion_dma_and_write_gate) {
  install_track(0, 0, 0, 0, 0xB1);
  setup_floppy();
  uint32_t expected = generation();
  ASSERT_EQ(floppy_flux_begin(&floppy, expected), BLOCK_OK);
  track_t track;
  fill_track(&track, 0, 0, 0xB2);
  ASSERT_EQ(floppy_write_track(&floppy, expected, &track), BLOCK_ERR_BUSY);
  ASSERT_EQ(floppy_seek(&floppy, 1), BLOCK_ERR_BUSY);
  ASSERT_EQ(floppy_select(&floppy, false), BLOCK_ERR_BUSY);
  ASSERT_EQ(floppy_deinit(&floppy), BLOCK_ERR_BUSY);
  ASSERT_EQ(drive.write_gate_assertions, 0);
  ASSERT_EQ(floppy_flux_end(&floppy), BLOCK_OK);
}

TEST(test_raw_flux_restart_resets_pc_and_blocks_idle_shutdown) {
  install_track(0, 0, 0, 0, 0xC2);
  setup_floppy();
  uint32_t expected = generation();
  uint32_t before_jumps = drive.read_restart_jumps;
  ASSERT_EQ(floppy_flux_begin(&floppy, expected), BLOCK_OK);
  pico_test_time_us += (uint64_t)(FLOPPY_IDLE_TIMEOUT_MS + 1u) * 1000u;
  ASSERT(pio_sim_fire_timer(&drive));
  ASSERT(floppy.motor_on);
  ASSERT(floppy.selected);
  ASSERT_EQ(floppy_flux_end(&floppy), BLOCK_OK);
  ASSERT(pio_sim_fire_timer(&drive));
  ASSERT(!floppy.motor_on);
  ASSERT(!floppy.selected);
  ASSERT_EQ(floppy_flux_begin(&floppy, expected), BLOCK_OK);
  ASSERT_EQ(floppy_flux_end(&floppy), BLOCK_OK);
  ASSERT_EQ(drive.read_restart_jumps - before_jumps, 2);
}

int main(void) {
  pio_sim_init(&drive);
  pio_sim_install(&drive);
  printf("=== PIO Simulator Tests ===\n\n");
  RUN_TEST(test_init_configures_hd_open_drain);
  RUN_TEST(test_public_read_entry_points_reject_null_and_uninitialized_instances);
  RUN_TEST(test_uninitialized_automatic_context_initializes);
  RUN_TEST(test_scp_loader_rejects_checksum_and_nonstandard_width);
  RUN_TEST(test_distinct_hardware_owners_are_exclusive_and_nonmutating);
  RUN_TEST(test_init_deinit_and_resource_failures_are_leak_free);
  RUN_TEST(test_read_sector_uses_zero_based_index);
  RUN_TEST(test_read_track_is_one_complete_revolution);
  RUN_TEST(test_seeded_drive_noise_reaches_the_hardware_path);
  RUN_TEST(test_track_read_preserves_crc_valid_partial_sectors);
  RUN_TEST(test_stray_wrong_id_does_not_abort_requested_read);
  RUN_TEST(test_duplicate_id_in_one_revolution_is_corrupt);
  RUN_TEST(test_wrong_ch_does_not_mask_requested_partial_track);
  RUN_TEST(test_conflicting_data_across_revolutions_is_corrupt);
  RUN_TEST(test_wrong_track_is_exact_terminal_status);
  RUN_TEST(test_persistent_wrong_track_rehomes_and_recovers);
  RUN_TEST(test_wrong_side_is_exact_terminal_status);
  RUN_TEST(test_media_generation_is_edge_triggered);
  RUN_TEST(test_media_generation_skips_zero_on_wrap);
  RUN_TEST(test_media_latch_is_cleared_and_spindle_is_qualified_per_generation);
  RUN_TEST(test_disk_change_output_is_gated_by_drive_selection);
  RUN_TEST(test_rx_overrun_is_explicit_and_restart_has_no_stale_data);
  RUN_TEST(test_transient_rx_overrun_recovers_from_fresh_stream);
  RUN_TEST(test_pio_rx_stall_is_explicit_and_recoverable);
  RUN_TEST(test_media_change_during_flux_is_linearized);
  RUN_TEST(test_seek_rejects_invalid_cylinder_without_moving);
  RUN_TEST(test_seek_applies_final_head_settle);
  RUN_TEST(test_motor_and_select_are_idempotent);
  RUN_TEST(test_motor_rejects_rapid_index_noise);
  RUN_TEST(test_raw_flux_session_is_generation_guarded);
  RUN_TEST(test_raw_flux_exclusively_owns_motion_dma_and_write_gate);
  RUN_TEST(test_raw_flux_restart_resets_pc_and_blocks_idle_shutdown);
  ASSERT_EQ(floppy_deinit(&floppy), BLOCK_OK);
  pio_sim_free(&drive);
  TEST_RESULTS();
}
