#include "test.h"
#include "vdisk.h"
#include "../src/crc.h"
#include "../src/mfm_decode.h"
#include "../src/mfm_encode.h"
#include "../src/fat12.h"

TEST(test_fat12_zero_sectors_per_track) {
  vdisk_t disk;
  vdisk_format_valid(&disk);
  disk.data[0][24] = 0;
  disk.data[0][25] = 0;

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_err_t err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_ERR_INVALID);
}

TEST(test_fat12_zero_num_heads) {
  vdisk_t disk;
  vdisk_format_valid(&disk);
  disk.data[0][26] = 0;
  disk.data[0][27] = 0;

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_err_t err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_ERR_INVALID);
}

TEST(test_fat12_missing_boot_signature) {
  vdisk_t disk;
  vdisk_format_valid(&disk);
  disk.data[0][510] = 0x00;
  disk.data[0][511] = 0x00;

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_err_t err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_ERR_INVALID);
}

TEST(test_fat12_zero_sectors_per_cluster) {
  vdisk_t disk;
  vdisk_format_valid(&disk);
  disk.data[0][13] = 0;

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_err_t err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_ERR_INVALID);
}

TEST(test_fat12_null_read_callback) {
  fat12_t fat;
  fat12_io_t io = { .read = NULL, .write = vdisk_write, .ctx = NULL };
  fat12_err_t err = fat12_init(&fat, io);
  ASSERT_EQ(err, FAT12_ERR_INVALID);
}

TEST(test_fat12_cluster_underflow) {
  vdisk_t disk;
  vdisk_format_valid(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  uint8_t buf[512];
  fat12_err_t err = fat12_read_cluster(&fat, 0, buf);
  ASSERT_EQ(err, FAT12_ERR_INVALID);

  err = fat12_read_cluster(&fat, 1, buf);
  ASSERT_EQ(err, FAT12_ERR_INVALID);
}

static uint16_t pulse_to_delta(uint8_t pulse) {
  return pulse + MFM_PIO_OVERHEAD;
}

TEST(test_mfm_decode_large_size_code) {
  mfm_t m;
  mfm_init(&m);

  uint8_t pulse_buf[8192];
  mfm_encode_t enc;
  mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

  uint8_t addr_data[] = {0xFE, 0x00, 0x00, 0x01, 0x03};
  uint16_t crc = 0xFFFF;
  crc = crc16_update(crc, 0xA1);
  crc = crc16_update(crc, 0xA1);
  crc = crc16_update(crc, 0xA1);
  for (int i = 0; i < 5; i++) {
    crc = crc16_update(crc, addr_data[i]);
  }

  mfm_encode_gap(&enc, 80);

  mfm_encode_sync(&enc);
  mfm_encode_bytes(&enc, addr_data, 5);
  uint8_t crc_bytes[2] = {crc >> 8, crc & 0xFF};
  mfm_encode_bytes(&enc, crc_bytes, 2);

  mfm_encode_gap(&enc, 22);

  uint8_t data_mark = 0xFB;
  uint8_t data_buf[512];
  memset(data_buf, 0x42, 512);

  uint16_t data_crc = 0xFFFF;
  data_crc = crc16_update(data_crc, 0xA1);
  data_crc = crc16_update(data_crc, 0xA1);
  data_crc = crc16_update(data_crc, 0xA1);
  data_crc = crc16_update(data_crc, data_mark);
  for (int i = 0; i < 512; i++) {
    data_crc = crc16_update(data_crc, data_buf[i]);
  }

  mfm_encode_sync(&enc);
  mfm_encode_bytes(&enc, &data_mark, 1);
  mfm_encode_bytes(&enc, data_buf, 512);
  uint8_t data_crc_bytes[2] = {data_crc >> 8, data_crc & 0xFF};
  mfm_encode_bytes(&enc, data_crc_bytes, 2);

  sector_t out;
  memset(&out, 0, sizeof(out));
  bool got_sector = false;

  for (size_t i = 0; i < enc.pos; i++) {
    if (mfm_feed(&m, pulse_to_delta(pulse_buf[i]), &out)) {
      got_sector = true;
    }
  }

  ASSERT(got_sector);
  ASSERT_EQ(out.size_code, 2);
  ASSERT(out.valid);
}

TEST(test_mfm_decode_invalid_pulses) {
  mfm_t m;
  mfm_init(&m);

  sector_t out;
  memset(&out, 0, sizeof(out));

  uint16_t garbage[] = {0, 1, 5, 10, 20, 30, 150, 200, 255, 1000, 65535};
  for (int i = 0; i < 11; i++) {
    bool got_sector = mfm_feed(&m, garbage[i], &out);
    ASSERT(!got_sector);
  }

  ASSERT(m.state == MFM_HUNT);
}

TEST(test_mfm_decode_truncated_sector) {
  mfm_t m;
  mfm_init(&m);

  uint8_t pulse_buf[4096];
  mfm_encode_t enc;
  mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

  sector_t s = {
    .track = 0, .side = 0, .sector_n = 1,
    .size_code = 2, .valid = true
  };
  memset(s.data, 0xAA, 512);

  uint8_t addr[5] = {0xFE, s.track, s.side, s.sector_n, 0x02};
  uint16_t addr_crc = crc16_mfm(addr, 5);

  mfm_encode_gap(&enc, 80);

  mfm_encode_sync(&enc);
  mfm_encode_bytes(&enc, addr, 5);
  uint8_t addr_crc_bytes[2] = {addr_crc >> 8, addr_crc & 0xFF};
  mfm_encode_bytes(&enc, addr_crc_bytes, 2);

  sector_t out;
  for (size_t i = 0; i < enc.pos; i++) {
    mfm_feed(&m, pulse_to_delta(pulse_buf[i]), &out);
  }

  ASSERT(m.have_pending_addr == true);
  ASSERT(m.sectors_read == 0);
}

TEST(test_mfm_decode_corrupted_crc) {
  mfm_t m;
  mfm_init(&m);

  uint8_t pulse_buf[8192];
  mfm_encode_t enc;
  mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

  sector_t s = {
    .track = 0, .side = 0, .sector_n = 1,
    .size_code = 2, .valid = true
  };
  memset(s.data, 0x55, 512);

  mfm_encode_sector(&enc, &s);

  sector_t out;
  memset(&out, 0, sizeof(out));

  for (size_t i = 0; i < enc.pos; i++) {
    uint16_t delta = pulse_to_delta(pulse_buf[i]);
    if (i == 50 || i == 51) delta = 0;
    mfm_feed(&m, delta, &out);
  }
}

TEST(test_mfm_encode_buffer_overflow) {
  uint8_t tiny_buf[10];
  mfm_encode_t enc;
  mfm_encode_init(&enc, tiny_buf, sizeof(tiny_buf));

  sector_t s = {
    .track = 0, .side = 0, .sector_n = 1,
    .size_code = 2, .valid = true
  };
  memset(s.data, 0, 512);

  mfm_encode_sector(&enc, &s);

  ASSERT(enc.pos <= sizeof(tiny_buf));
}

TEST(test_mfm_decode_rapid_state_changes) {
  mfm_t m;
  mfm_init(&m);

  sector_t out;

  for (int i = 0; i < 1000; i++) {
    uint16_t pulse = (i % 3 == 0) ? 48 : (i % 3 == 1) ? 72 : 96;
    mfm_feed(&m, pulse, &out);
  }

  ASSERT(m.state >= MFM_HUNT && m.state <= MFM_CLOCK);
}

TEST(test_mfm_encode_null_sector) {
  uint8_t buf[4096];
  mfm_encode_t enc;
  mfm_encode_init(&enc, buf, sizeof(buf));

  sector_t s;
  memset(&s, 0, sizeof(s));

  mfm_encode_sector(&enc, &s);

  ASSERT(enc.pos > 0);
}

int main(void) {
  printf("=== Robustness Tests ===\n\n");

  printf("--- FAT12 Edge Cases ---\n");
  RUN_TEST(test_fat12_zero_sectors_per_track);
  RUN_TEST(test_fat12_zero_num_heads);
  RUN_TEST(test_fat12_missing_boot_signature);
  RUN_TEST(test_fat12_zero_sectors_per_cluster);
  RUN_TEST(test_fat12_null_read_callback);
  RUN_TEST(test_fat12_cluster_underflow);

  printf("\n--- MFM Decoder Edge Cases ---\n");
  RUN_TEST(test_mfm_decode_large_size_code);
  RUN_TEST(test_mfm_decode_invalid_pulses);
  RUN_TEST(test_mfm_decode_truncated_sector);
  RUN_TEST(test_mfm_decode_corrupted_crc);
  RUN_TEST(test_mfm_encode_buffer_overflow);
  RUN_TEST(test_mfm_decode_rapid_state_changes);
  RUN_TEST(test_mfm_encode_null_sector);

  TEST_RESULTS();
}
