#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "../src/floppy.h"
#include "../src/mfm_decode.h"
#include "../src/mfm_encode.h"

// Remap encoder pulse values to decoder-compatible values
void remap_pulses(uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    // Encoder: SHORT=29, MEDIUM=53, LONG=77
    // Decoder expects: SHORT ~47, MEDIUM ~70, LONG ~95
    if (buf[i] == 29) buf[i] = 47;       // SHORT
    else if (buf[i] == 53) buf[i] = 70;  // MEDIUM
    else if (buf[i] == 77) buf[i] = 95;  // LONG
  }
}

int main(void) {
  mfm_t m;
  mfm_init(&m);

  uint8_t pulse_buf[8192];
  mfm_encode_t enc;
  mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

  uint8_t addr_data[] = {0xFE, 0x00, 0x00, 0x01, 0x03};  // size_code = 3
  uint16_t crc = 0xFFFF;
  crc = crc16_update(crc, 0xA1);
  crc = crc16_update(crc, 0xA1);
  crc = crc16_update(crc, 0xA1);
  for (int i = 0; i < 5; i++) {
    crc = crc16_update(crc, addr_data[i]);
  }

  // Need gap before sync for preamble
  mfm_encode_gap(&enc, 80);

  // Send address with size_code 3
  mfm_encode_sync(&enc);
  mfm_encode_bytes(&enc, addr_data, 5);
  uint8_t crc_bytes[2] = {crc >> 8, crc & 0xFF};
  mfm_encode_bytes(&enc, crc_bytes, 2);

  // Gap between address and data
  mfm_encode_gap(&enc, 22);

  // Send data for 512 bytes (clamped size)
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

  printf("Encoded %zu pulses\n", enc.pos);

  // Remap pulse values to decoder-compatible range
  remap_pulses(pulse_buf, enc.pos);

  // Feed to decoder
  sector_t out;
  memset(&out, 0, sizeof(out));
  bool got_sector = false;

  for (size_t i = 0; i < enc.pos; i++) {
    if (mfm_feed(&m, pulse_buf[i], &out)) {
      got_sector = true;
      printf("Got sector at pulse %zu!\n", i);
    }
  }

  printf("Syncs found: %u\n", m.syncs_found);
  printf("Sectors read: %u\n", m.sectors_read);
  printf("CRC errors: %u\n", m.crc_errors);
  printf("have_pending_addr: %d\n", m.have_pending_addr);
  printf("pending_size_code: %u\n", m.pending_size_code);
  printf("bytes_expected: %u\n", m.bytes_expected);
  printf("buf_pos: %u\n", m.buf_pos);
  printf("got_sector: %d\n", got_sector);

  if (got_sector) {
    printf("out.size_code: %u\n", out.size_code);
    printf("out.valid: %d\n", out.valid);
  }

  return 0;
}
