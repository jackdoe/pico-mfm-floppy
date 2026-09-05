#include "test.h"
#include "../src/crc.h"
#include "../src/mfm.h"
#include <string.h>

static void encode_address(mfm_encode_t *encoder, uint8_t cylinder, uint8_t head,
                           uint8_t sector, uint8_t size_code, bool bad_crc) {
  uint8_t address[] = {MFM_ADDR_MARK, cylinder, head, sector, size_code};
  uint16_t crc = crc16(address, sizeof(address), MFM_CRC_INIT);
  if (bad_crc) crc ^= 1u;
  uint8_t crc_bytes[] = {(uint8_t)(crc >> 8), (uint8_t)(crc & 0xFF)};
  mfm_encode_sync(encoder);
  mfm_encode_bytes(encoder, address, sizeof(address));
  mfm_encode_bytes(encoder, crc_bytes, sizeof(crc_bytes));
}

static void encode_data(mfm_encode_t *encoder, uint8_t mark, const uint8_t *data,
                        size_t size, bool bad_crc) {
  uint16_t crc = crc16(data, size, crc16_update(MFM_CRC_INIT, mark));
  if (bad_crc) crc ^= 1u;
  uint8_t crc_bytes[] = {(uint8_t)(crc >> 8), (uint8_t)(crc & 0xFF)};
  mfm_encode_sync(encoder);
  mfm_encode_bytes(encoder, &mark, 1);
  mfm_encode_bytes(encoder, data, size);
  mfm_encode_bytes(encoder, crc_bytes, sizeof(crc_bytes));
}

static bool decode_all(mfm_t *decoder, const uint8_t *pulses, size_t count,
                       mfm_sector_t *sector) {
  bool found = false;
  for (size_t i = 0; i < count; i++) {
    if (mfm_feed(decoder, pulses[i], sector)) found = true;
  }
  return found;
}

static void fill_track(track_t *track, uint8_t cylinder, uint8_t head, uint32_t seed) {
  memset(track, 0, sizeof(*track));
  track->cylinder = cylinder;
  track->head = head;
  track->valid = DISK_TRACK_VALID;
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    for (size_t byte = 0; byte < DISK_SECTOR_SIZE; byte++) {
      seed = seed * 1664525u + 1013904223u;
      track->data[sector][byte] = (uint8_t)(seed >> 24);
    }
  }
}

static uint16_t reference_mfm_word(uint8_t byte, bool previous) {
  uint16_t data = 0;
  for (unsigned bit = 0; bit < 8; bit++) {
    data |= (uint16_t)(((uint32_t)byte >> bit & 1u) << (bit * 2u));
  }
  uint16_t clocks = (uint16_t)(~(((uint32_t)data << 1u) | ((uint32_t)data >> 1u)) & 0xAAAAu);
  if (previous) clocks &= 0x7FFFu;
  return (uint16_t)(data | clocks);
}

static void reference_word_intervals(uint16_t word, unsigned *position,
                                     unsigned *previous, uint8_t *out,
                                     size_t *count) {
  for (int bit = 15; bit >= 0; bit--) {
    *position += MFM_CELL_TICKS / 2u;
    if ((word & (1u << bit)) == 0) continue;
    unsigned interval = *position - *previous;
    out[(*count)++] = (uint8_t)(interval);
    *previous = *position;
  }
}

TEST(test_encoder_matches_bitcell_oracle_for_every_byte_pair) {
  for (uint32_t pair = 0; pair <= UINT16_MAX; pair++) {
    uint8_t data[] = {0, (uint8_t)(pair >> 8), (uint8_t)pair};
    uint8_t expected[32];
    unsigned position = MFM_CELL_TICKS / 2u;
    unsigned previous = 0;
    size_t count = 0;
    bool previous_bit = false;
    for (size_t i = 0; i < sizeof(data); i++) {
      reference_word_intervals(reference_mfm_word(data[i], previous_bit),
                               &position, &previous, expected, &count);
      previous_bit = (data[i] & 1u) != 0;
    }
    uint8_t actual[32];
    mfm_encode_t encoder;
    mfm_encode_init(&encoder, actual, sizeof(actual));
    mfm_encode_bytes(&encoder, data, sizeof(data));
    ASSERT_EQ(encoder.pos, count);
    ASSERT_MEM_EQ(actual, expected, count);
  }
}

TEST(test_sync_matches_three_missing_clock_a1_words) {
  uint8_t expected[128];
  unsigned position = MFM_CELL_TICKS / 2u;
  unsigned previous = 0;
  size_t count = 0;
  for (unsigned i = 0; i < 12; i++) {
    reference_word_intervals(0xAAAAu, &position, &previous, expected, &count);
  }
  for (unsigned i = 0; i < 3; i++) {
    reference_word_intervals(0x4489u, &position, &previous, expected, &count);
  }
  uint8_t actual[128];
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, actual, sizeof(actual));
  mfm_encode_sync(&encoder);
  ASSERT_EQ(encoder.pos, count);
  ASSERT_MEM_EQ(actual, expected, count);
}

TEST(test_encoder_sync) {
  uint8_t pulses[1024];
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_sync(&encoder);
  ASSERT(encoder.pos > 100);
  static const uint8_t expected[] = {
      MFM_PULSE_MEDIUM, MFM_PULSE_LONG, MFM_PULSE_MEDIUM,
      MFM_PULSE_LONG, MFM_PULSE_MEDIUM, MFM_PULSE_SHORT,
      MFM_PULSE_LONG, MFM_PULSE_MEDIUM, MFM_PULSE_LONG,
      MFM_PULSE_MEDIUM, MFM_PULSE_SHORT, MFM_PULSE_LONG,
      MFM_PULSE_MEDIUM, MFM_PULSE_LONG, MFM_PULSE_MEDIUM,
  };
  ASSERT_MEM_EQ(&pulses[encoder.pos - sizeof(expected)], expected, sizeof(expected));
}

TEST(test_crc) {
  static const uint8_t marks[] = {0xA1, 0xA1, 0xA1};
  ASSERT_EQ(crc16(marks, sizeof(marks), 0xFFFF), MFM_CRC_INIT);
  ASSERT_EQ(MFM_PULSE_FLOOR, 38);
  ASSERT_EQ(MFM_PULSE_CEILING, 120);
  ASSERT_EQ(MFM_PULSE_SHORT, MFM_CELL_TICKS);
  ASSERT_EQ(MFM_READ_PIO_HZ, 72000000u);
}

TEST(test_roundtrip_sector) {
  uint8_t pulses[16384];
  uint8_t data[DISK_SECTOR_SIZE];
  for (size_t i = 0; i < sizeof(data); i++) data[i] = (i * 37u + 11u) & 0xFFu;
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_sector(&encoder, 10, 1, 6, data);
  mfm_encode_gap(&encoder, 10);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  ASSERT(decode_all(&decoder, pulses, encoder.pos, &sector));
  ASSERT_EQ(sector.cylinder, 10);
  ASSERT_EQ(sector.head, 1);
  ASSERT_EQ(sector.sector, 6);
  ASSERT_MEM_EQ(sector.data, data, sizeof(data));
  ASSERT_EQ(decoder.crc_errors, 0);
  ASSERT_EQ(decoder.format_errors, 0);
}

TEST(test_roundtrip_patterns) {
  static const uint8_t patterns[] = {0x00, 0xFF, 0xAA, 0x55, 0x81, 0x7E};
  for (size_t pattern = 0; pattern < sizeof(patterns); pattern++) {
    uint8_t pulses[16384];
    uint8_t data[DISK_SECTOR_SIZE];
    memset(data, patterns[pattern], sizeof(data));
    mfm_encode_t encoder;
    mfm_encode_init(&encoder, pulses, sizeof(pulses));
    mfm_encode_sector(&encoder, 0, 0, 0, data);
    mfm_encode_gap(&encoder, 10);
    mfm_t decoder;
    mfm_sector_t sector;
    mfm_init(&decoder);
    ASSERT(decode_all(&decoder, pulses, encoder.pos, &sector));
    ASSERT_MEM_EQ(sector.data, data, sizeof(data));
  }
}

TEST(test_roundtrip_with_jitter) {
  uint8_t pulses[16384];
  uint8_t data[DISK_SECTOR_SIZE];
  for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)i;
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_sector(&encoder, 3, 0, 12, data);
  mfm_encode_gap(&encoder, 10);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  uint32_t seed = 0x12345678u;
  bool found = false;
  for (size_t i = 0; i < encoder.pos; i++) {
    seed = seed * 1103515245u + 12345u;
    int jitter = (int)((seed >> 16) % 11u) - 5;
    uint16_t delta = (uint16_t)((int)pulses[i] + jitter);
    if (mfm_feed(&decoder, delta, &sector)) found = true;
  }
  ASSERT(found);
  ASSERT_MEM_EQ(sector.data, data, sizeof(data));
}

TEST(test_roundtrip_full_track) {
  static uint8_t pulses[200000];
  track_t track;
  fill_track(&track, 5, 1, 0xC0FFEEu);
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_track(&encoder, &track);
  ASSERT(!encoder.overflow);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  uint32_t seen = 0;
  for (size_t i = 0; i < encoder.pos; i++) {
    if (!mfm_feed(&decoder, pulses[i], &sector)) continue;
    ASSERT_EQ(sector.cylinder, track.cylinder);
    ASSERT_EQ(sector.head, track.head);
    ASSERT_MEM_EQ(sector.data, track.data[sector.sector], DISK_SECTOR_SIZE);
    seen |= 1u << sector.sector;
  }
  ASSERT_EQ(seen, DISK_TRACK_VALID);
  ASSERT_EQ(decoder.sectors_read, DISK_SECTORS_PER_TRACK);
}

TEST(test_deleted_data_mark_f8) {
  uint8_t pulses[16384];
  uint8_t data[DISK_SECTOR_SIZE];
  memset(data, 0x77, sizeof(data));
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  encode_address(&encoder, 3, 0, 2, MFM_SIZE_CODE, false);
  mfm_encode_gap(&encoder, 22);
  encode_data(&encoder, MFM_DELETED_MARK, data, sizeof(data), false);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  ASSERT(decode_all(&decoder, pulses, encoder.pos, &sector));
  ASSERT_EQ(sector.sector, 1);
  ASSERT_MEM_EQ(sector.data, data, sizeof(data));
}

TEST(test_legacy_fa_mark_rejected) {
  uint8_t pulses[16384];
  uint8_t data[DISK_SECTOR_SIZE] = {0};
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  encode_address(&encoder, 0, 0, 1, MFM_SIZE_CODE, false);
  mfm_encode_gap(&encoder, 22);
  encode_data(&encoder, 0xFA, data, sizeof(data), false);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  ASSERT(!decode_all(&decoder, pulses, encoder.pos, &sector));
  ASSERT(decoder.format_errors > 0);
  ASSERT_EQ(decoder.record_state, MFM_EXPECT_ID);
}

TEST(test_only_size_code_two_is_accepted) {
  for (uint8_t size_code = 0; size_code < 4; size_code++) {
    uint8_t pulses[4096];
    mfm_encode_t encoder;
    mfm_encode_init(&encoder, pulses, sizeof(pulses));
    encode_address(&encoder, 0, 0, 1, size_code, false);
    mfm_encode_gap(&encoder, 4);
    mfm_t decoder;
    mfm_sector_t sector;
    mfm_init(&decoder);
    ASSERT(!decode_all(&decoder, pulses, encoder.pos, &sector));
    if (size_code == MFM_SIZE_CODE) {
      ASSERT_EQ(decoder.record_state, MFM_EXPECT_DATA);
    } else {
      ASSERT_EQ(decoder.record_state, MFM_EXPECT_ID);
      ASSERT(decoder.format_errors > 0);
    }
  }
}

TEST(test_invalid_chrn_is_rejected) {
  static const uint8_t address[][4] = {
      {DISK_CYLINDERS, 0, 1, MFM_SIZE_CODE},
      {0, DISK_HEADS, 1, MFM_SIZE_CODE},
      {0, 0, 0, MFM_SIZE_CODE},
      {0, 0, DISK_SECTORS_PER_TRACK + 1u, MFM_SIZE_CODE},
  };
  for (size_t i = 0; i < sizeof(address) / sizeof(address[0]); i++) {
    uint8_t pulses[4096];
    mfm_encode_t encoder;
    mfm_encode_init(&encoder, pulses, sizeof(pulses));
    encode_address(&encoder, address[i][0], address[i][1], address[i][2],
                   address[i][3], false);
    mfm_encode_gap(&encoder, 4);
    mfm_t decoder;
    mfm_sector_t sector;
    mfm_init(&decoder);
    ASSERT(!decode_all(&decoder, pulses, encoder.pos, &sector));
    ASSERT_EQ(decoder.record_state, MFM_EXPECT_ID);
    ASSERT(decoder.format_errors > 0);
  }
}

TEST(test_address_crc_error_drops_record) {
  uint8_t pulses[4096];
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  encode_address(&encoder, 0, 0, 1, MFM_SIZE_CODE, true);
  mfm_encode_gap(&encoder, 4);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  ASSERT(!decode_all(&decoder, pulses, encoder.pos, &sector));
  ASSERT_EQ(decoder.crc_errors, 1);
  ASSERT_EQ(decoder.record_state, MFM_EXPECT_ID);
}

TEST(test_data_crc_error_is_not_emitted) {
  uint8_t pulses[16384];
  uint8_t data[DISK_SECTOR_SIZE];
  memset(data, 0xA5, sizeof(data));
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  encode_address(&encoder, 0, 0, 1, MFM_SIZE_CODE, false);
  mfm_encode_gap(&encoder, 22);
  encode_data(&encoder, MFM_DATA_MARK, data, sizeof(data), true);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  ASSERT(!decode_all(&decoder, pulses, encoder.pos, &sector));
  ASSERT_EQ(decoder.crc_errors, 1);
  ASSERT_EQ(decoder.sectors_read, 0);
  ASSERT_EQ(decoder.record_state, MFM_EXPECT_ID);
}

TEST(test_new_address_while_waiting_for_data_is_rejected) {
  uint8_t pulses[8192];
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  encode_address(&encoder, 0, 0, 1, MFM_SIZE_CODE, false);
  mfm_encode_gap(&encoder, 22);
  encode_address(&encoder, 0, 0, 2, MFM_SIZE_CODE, false);
  mfm_encode_gap(&encoder, 4);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  ASSERT(!decode_all(&decoder, pulses, encoder.pos, &sector));
  ASSERT_EQ(decoder.record_state, MFM_EXPECT_ID);
  ASSERT(decoder.format_errors > 0);
}

TEST(test_preamble_accumulator_resets_before_overflow) {
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  decoder.short_count = UINT16_MAX;
  decoder.preamble_sum = UINT32_MAX - 10u;
  ASSERT(!mfm_feed(&decoder, 48, &sector));
  ASSERT_EQ(decoder.short_count, 1);
  ASSERT_EQ(decoder.preamble_sum, 48);
}

TEST(test_invalid_pulse_drops_pending_address) {
  uint8_t pulses[4096];
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  encode_address(&encoder, 0, 0, 1, MFM_SIZE_CODE, false);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  decode_all(&decoder, pulses, encoder.pos, &sector);
  ASSERT_EQ(decoder.record_state, MFM_EXPECT_DATA);
  ASSERT(!mfm_feed(&decoder, 0, &sector));
  ASSERT_EQ(decoder.record_state, MFM_EXPECT_ID);
  ASSERT_EQ(decoder.state, MFM_HUNT);
}

TEST(test_malformed_sync_drops_pending_address) {
  uint8_t pulses[4096];
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  encode_address(&encoder, 0, 0, 1, MFM_SIZE_CODE, false);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  decode_all(&decoder, pulses, encoder.pos, &sector);
  ASSERT_EQ(decoder.record_state, MFM_EXPECT_DATA);
  for (unsigned i = 0; i < MFM_MIN_PREAMBLE; i++) {
    ASSERT(!mfm_feed(&decoder, 48, &sector));
  }
  ASSERT(!mfm_feed(&decoder, 96, &sector));
  ASSERT_EQ(decoder.record_state, MFM_EXPECT_ID);
  ASSERT(decoder.format_errors > 0);
}

TEST(test_address_to_data_distance_is_bounded) {
  uint8_t pulses[4096];
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  encode_address(&encoder, 0, 0, 1, MFM_SIZE_CODE, false);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  decode_all(&decoder, pulses, encoder.pos, &sector);
  ASSERT_EQ(decoder.record_state, MFM_EXPECT_DATA);
  for (unsigned i = 0; i <= MFM_ID_DATA_MAX_PULSES; i++) {
    mfm_feed(&decoder, 48, &sector);
  }
  ASSERT_EQ(decoder.record_state, MFM_EXPECT_ID);
  ASSERT(decoder.format_errors > 0);
}

TEST(test_data_without_address_is_rejected) {
  uint8_t pulses[16384];
  uint8_t data[DISK_SECTOR_SIZE] = {0};
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  encode_data(&encoder, MFM_DATA_MARK, data, sizeof(data), false);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  ASSERT(!decode_all(&decoder, pulses, encoder.pos, &sector));
  ASSERT_EQ(decoder.record_state, MFM_EXPECT_ID);
}

TEST(test_encoder_buffer_overflow_stops) {
  uint8_t buffer[10];
  uint8_t data[DISK_SECTOR_SIZE] = {0};
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, buffer, sizeof(buffer));
  mfm_encode_sector(&encoder, 0, 0, 0, data);
  ASSERT_EQ(encoder.pos, sizeof(buffer));
  ASSERT(encoder.overflow);
  ASSERT(encoder.stopped);
}

TEST(test_encoder_rejects_invalid_inputs) {
  uint8_t pulses[1024];
  uint8_t data[DISK_SECTOR_SIZE] = {0};
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_sector(&encoder, DISK_CYLINDERS, 0, 0, data);
  ASSERT(encoder.stopped);

  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_sector(&encoder, 0, DISK_HEADS, 0, data);
  ASSERT(encoder.stopped);

  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_sector(&encoder, 0, 0, DISK_SECTORS_PER_TRACK, data);
  ASSERT(encoder.stopped);

  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_sector(&encoder, 0, 0, 0, NULL);
  ASSERT(encoder.stopped);

  mfm_encode_init(&encoder, NULL, sizeof(pulses));
  ASSERT(encoder.stopped);
  ASSERT(encoder.overflow);

  mfm_encode_init_emit(&encoder, NULL, NULL);
  ASSERT(encoder.stopped);

  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  ASSERT_EQ(mfm_encode_track(&encoder, NULL), 0);
  ASSERT(encoder.stopped);
}

static void reference_precomp(uint8_t *pulses, size_t count, uint8_t cylinder) {
  static uint8_t bits[DISK_SECTOR_SIZE * DISK_SECTORS_PER_TRACK * 32u];
  static uint32_t edges[200000];
  memset(bits, 0, sizeof(bits));
  bits[0] = 1;
  uint32_t position = 0;
  for (size_t i = 0; i < count; i++) {
    unsigned ticks = pulses[i];
    ASSERT_EQ(ticks % (MFM_CELL_TICKS / 2u), 0);
    position += ticks / (MFM_CELL_TICKS / 2u);
    ASSERT(position + 2u < sizeof(bits));
    bits[position] = 1;
    edges[i] = position;
  }
  int shift = cylinder >= MFM_PRECOMP_START_TRACK ? MFM_PRECOMP_SHIFT : 0;
  int previous = 0;
  for (size_t i = 0; i < count; i++) {
    uint32_t edge = edges[i];
    unsigned window = 0;
    for (uint32_t bit = edge - 2u; bit <= edge + 2u; bit++) {
      window = (window << 1u) | bits[bit];
    }
    int adjusted = (int)(edge * (MFM_CELL_TICKS / 2u));
    if (i + 1u < count) {
      if (window == 0x14u) adjusted -= shift;
      if (window == 0x05u) adjusted += shift;
    }
    int interval = adjusted - previous;
    ASSERT(interval > 0 && interval <= UINT8_MAX);
    pulses[i] = (uint8_t)interval;
    previous = adjusted;
  }
}

TEST(test_streaming_precomp_matches_reference) {
  static uint8_t expected[200000];
  static uint8_t actual[200000];
  for (uint8_t cylinder = MFM_PRECOMP_START_TRACK; cylinder < DISK_CYLINDERS;
       cylinder += 13) {
    track_t track;
    fill_track(&track, cylinder, 1, 0xBEEFu + cylinder);
    mfm_encode_t encoder;
    mfm_encode_init(&encoder, expected, sizeof(expected));
    mfm_encode_gap(&encoder, 80);
    for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
      mfm_encode_sector(&encoder, cylinder, 1, sector, track.data[sector]);
      mfm_encode_gap(&encoder, 54);
    }
    size_t count = encoder.pos;
    reference_precomp(expected, count, cylinder);
    mfm_encode_init(&encoder, actual, sizeof(actual));
    ASSERT_EQ(mfm_encode_track(&encoder, &track), count);
    ASSERT_MEM_EQ(actual, expected, count);
  }
}

TEST(test_precompensation_moves_edges_without_accumulating_drift) {
  static uint8_t nominal[200000];
  static uint8_t compensated[200000];
  track_t track;
  fill_track(&track, 79, 0, 1234);
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, nominal, sizeof(nominal));
  mfm_encode_gap(&encoder, 80);
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    mfm_encode_sector(&encoder, track.cylinder, track.head, sector, track.data[sector]);
    mfm_encode_gap(&encoder, 54);
  }
  size_t count = encoder.pos;
  mfm_encode_init(&encoder, compensated, sizeof(compensated));
  ASSERT_EQ(mfm_encode_track(&encoder, &track), count);
  int limit = MFM_PRECOMP_SHIFT;
  int displacement = 0;
  bool early = false;
  bool late = false;
  for (size_t i = 0; i < count; i++) {
    displacement += (int)compensated[i] - (int)nominal[i];
    ASSERT(displacement >= -limit && displacement <= limit);
    early |= displacement < 0;
    late |= displacement > 0;
  }
  ASSERT(early && late);
  ASSERT_EQ(displacement, 0);
}

typedef struct {
  uint8_t *pulses;
  size_t count;
  size_t limit;
} capture_t;

static bool capture_emit(void *ctx, uint8_t pulse) {
  capture_t *capture = ctx;
  if (capture->count == capture->limit) return false;
  capture->pulses[capture->count++] = pulse;
  return true;
}

TEST(test_emit_mode_matches_linear_mode) {
  static uint8_t linear[200000];
  static uint8_t streamed[200000];
  track_t track;
  fill_track(&track, 70, 1, 0x1234u);
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, linear, sizeof(linear));
  size_t expected = mfm_encode_track(&encoder, &track);
  capture_t capture = {.pulses = streamed, .limit = sizeof(streamed)};
  mfm_encode_init_emit(&encoder, capture_emit, &capture);
  size_t actual = mfm_encode_track(&encoder, &track);
  ASSERT_EQ(actual, expected);
  ASSERT_EQ(capture.count, expected);
  ASSERT_MEM_EQ(streamed, linear, expected);
}

TEST(test_emit_can_stop_encoder) {
  uint8_t pulses[32];
  uint8_t data[DISK_SECTOR_SIZE] = {0};
  capture_t capture = {.pulses = pulses, .limit = sizeof(pulses)};
  mfm_encode_t encoder;
  mfm_encode_init_emit(&encoder, capture_emit, &capture);
  mfm_encode_sector(&encoder, 0, 0, 0, data);
  ASSERT(encoder.stopped);
  ASSERT_EQ(encoder.pos, sizeof(pulses));
  ASSERT_EQ(capture.count, sizeof(pulses));
}

TEST(test_decoder_tracks_slow_changes_in_all_pulse_classes) {
  static const uint8_t patterns[] = {0x00, 0xFF, 0x55, 0x42};
  for (size_t pattern = 0; pattern < sizeof(patterns); pattern++) {
    uint8_t data[DISK_SECTOR_SIZE];
    memset(data, patterns[pattern], sizeof(data));
    uint8_t pulses[16384];
    mfm_encode_t encoder;
    mfm_encode_init(&encoder, pulses, sizeof(pulses));
    mfm_encode_sector(&encoder, 0, 0, 0, data);
    mfm_encode_gap(&encoder, 10);
    for (int direction = -1; direction <= 1; direction += 2) {
      mfm_t decoder;
      mfm_sector_t sector;
      mfm_init(&decoder);
      bool found = false;
      for (size_t i = 0; i < encoder.pos; i++) {
        int32_t change = (int32_t)(1600u * i / encoder.pos) * direction;
        uint32_t scale = (uint32_t)(10000 + change);
        uint16_t delta = (uint16_t)((pulses[i] * scale + 5000u) / 10000u);
        if (mfm_feed(&decoder, delta, &sector)) found = true;
      }
      ASSERT(found);
      ASSERT_MEM_EQ(data, sector.data, sizeof(data));
      uint32_t expected = direction > 0 ? 56u : 40u;
      ASSERT(decoder.cell_q8 >= (expected - 2u) * 256u);
      ASSERT(decoder.cell_q8 <= (expected + 2u) * 256u);
      ASSERT_EQ(decoder.crc_errors, 0);
    }
  }
}

TEST(test_invalid_pulses_break_preamble_continuity) {
  static const uint16_t invalid[] = {0, MFM_PULSE_FLOOR - 1u, MFM_PULSE_CEILING, UINT16_MAX};
  for (size_t j = 0; j < sizeof(invalid) / sizeof(invalid[0]); j++) {
    mfm_t decoder;
    mfm_sector_t sector;
    mfm_init(&decoder);
    for (size_t i = 0; i < MFM_MIN_PREAMBLE - 1u; i++) {
      ASSERT(!mfm_feed(&decoder, MFM_CELL_TICKS, &sector));
    }
    ASSERT(!mfm_feed(&decoder, invalid[j], &sector));
    ASSERT_EQ(decoder.short_count, 0);
    ASSERT_EQ(decoder.preamble_sum, 0);
    ASSERT(!mfm_feed(&decoder, MFM_CELL_TICKS, &sector));
    ASSERT(!mfm_feed(&decoder, MFM_CELL_TICKS * 3u / 2u, &sector));
    ASSERT_EQ(decoder.state, MFM_HUNT);
    ASSERT_EQ(decoder.syncs_found, 0);
  }
}

TEST(test_decoder_null_inputs_fail_closed) {
  mfm_init(NULL);
  mfm_reset(NULL);
  mfm_t decoder;
  mfm_init(&decoder);
  mfm_t before = decoder;
  ASSERT(!mfm_feed(NULL, 48, NULL));
  ASSERT(!mfm_feed(&decoder, 48, NULL));
  ASSERT_MEM_EQ(&decoder, &before, sizeof(decoder));
}

int main(void) {
  printf("=== MFM Encoder/Decoder Tests ===\n\n");
  RUN_TEST(test_encoder_matches_bitcell_oracle_for_every_byte_pair);
  RUN_TEST(test_sync_matches_three_missing_clock_a1_words);
  RUN_TEST(test_encoder_sync);
  RUN_TEST(test_crc);
  RUN_TEST(test_roundtrip_sector);
  RUN_TEST(test_roundtrip_patterns);
  RUN_TEST(test_roundtrip_with_jitter);
  RUN_TEST(test_roundtrip_full_track);
  RUN_TEST(test_deleted_data_mark_f8);
  RUN_TEST(test_legacy_fa_mark_rejected);
  RUN_TEST(test_only_size_code_two_is_accepted);
  RUN_TEST(test_invalid_chrn_is_rejected);
  RUN_TEST(test_address_crc_error_drops_record);
  RUN_TEST(test_data_crc_error_is_not_emitted);
  RUN_TEST(test_new_address_while_waiting_for_data_is_rejected);
  RUN_TEST(test_preamble_accumulator_resets_before_overflow);
  RUN_TEST(test_invalid_pulse_drops_pending_address);
  RUN_TEST(test_malformed_sync_drops_pending_address);
  RUN_TEST(test_address_to_data_distance_is_bounded);
  RUN_TEST(test_data_without_address_is_rejected);
  RUN_TEST(test_encoder_buffer_overflow_stops);
  RUN_TEST(test_encoder_rejects_invalid_inputs);
  RUN_TEST(test_precompensation_moves_edges_without_accumulating_drift);
  RUN_TEST(test_streaming_precomp_matches_reference);
  RUN_TEST(test_emit_mode_matches_linear_mode);
  RUN_TEST(test_emit_can_stop_encoder);
  RUN_TEST(test_decoder_tracks_slow_changes_in_all_pulse_classes);
  RUN_TEST(test_invalid_pulses_break_preamble_continuity);
  RUN_TEST(test_decoder_null_inputs_fail_closed);
  TEST_RESULTS();
}
