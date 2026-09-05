#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

#include "board.h"
#include "f12.h"
#include "floppy.h"
#include "rpc_server.h"
#include "spi_link.h"

static floppy_t floppy;
static f12_t fs;
static spi_link_t link;
static uint8_t req_buf[RPC_MAX_MSG];
static uint8_t resp_buf[RPC_MAX_MSG];

static void link_gpio_init(void) {
  gpio_set_function(SPI_LINK_SLAVE_RX, GPIO_FUNC_SPI);
  gpio_set_function(SPI_LINK_SLAVE_SCK, GPIO_FUNC_SPI);
  gpio_set_function(SPI_LINK_SLAVE_TX, GPIO_FUNC_SPI);
  gpio_pull_down(SPI_LINK_SLAVE_SCK);
  gpio_set_function(SPI_LINK_SLAVE_CSN, GPIO_FUNC_SPI);
  gpio_set_inover(SPI_LINK_SLAVE_CSN, GPIO_OVERRIDE_LOW);
  gpio_init(SPI_LINK_SLAVE_CS);
  gpio_set_dir(SPI_LINK_SLAVE_CS, GPIO_IN);
  gpio_pull_up(SPI_LINK_SLAVE_CS);
}

static void wire_exchange(const uint8_t *frame, uint8_t *rx_frame) {
  uint8_t wtx[SPI_LINK_WIRE];
  uint8_t wrx[SPI_LINK_WIRE];
  memcpy(wtx, frame, SPI_LINK_FRAME);
  wtx[SPI_LINK_FRAME] = 0;

  while (!gpio_get(SPI_LINK_SLAVE_CS)) {
    floppy_poll(&floppy);
    tight_loop_contents();
  }

  spi_hw_t *hw = spi_get_hw(SPI_LINK_SLAVE_SPI);
  spi_init(SPI_LINK_SLAVE_SPI, SPI_LINK_HZ);
  hw_clear_bits(&hw->cr1, SPI_SSPCR1_SSE_BITS);
  spi_set_slave(SPI_LINK_SLAVE_SPI, true);
  spi_set_format(SPI_LINK_SLAVE_SPI, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);

  uint32_t tx_i = 0;
  uint32_t rx_i = 0;
  while (tx_i < 8) hw->dr = wtx[tx_i++];
  hw_set_bits(&hw->cr1, SPI_SSPCR1_SSE_BITS);

  while (rx_i < SPI_LINK_WIRE) {
    if (tx_i < SPI_LINK_WIRE && (hw->sr & SPI_SSPSR_TNF_BITS)) hw->dr = wtx[tx_i++];
    if (hw->sr & SPI_SSPSR_RNE_BITS) wrx[rx_i++] = (uint8_t)hw->dr;
  }

  memcpy(rx_frame, wrx, SPI_LINK_FRAME);
}

static void xfer(void) {
  uint8_t tx[SPI_LINK_FRAME];
  uint8_t rx[SPI_LINK_FRAME];
  spi_link_build(&link, tx);
  wire_exchange(tx, rx);
  if (spi_link_frame_ok(rx)) spi_link_accept(&link, rx);
}

static void send_bytes(const uint8_t *data, uint32_t count) {
  while (count) {
    while (link.pending_len) xfer();
    uint8_t chunk = spi_link_stage(&link, data, count);
    data += chunk;
    count -= chunk;
  }
  while (link.pending_len) xfer();
}

static void recv_bytes(uint8_t *data, uint32_t count) {
  uint32_t got = 0;
  for (;;) {
    got += spi_link_drain(&link, data + got, count - got);
    if (got >= count) return;
    xfer();
  }
}

static disk_result_t dispatch(uint8_t op, const uint8_t *req, uint16_t len,
                              uint8_t *resp) {
  disk_err_t error = DISK_ERR_INVALID;
  switch (op) {
    case RPC_STATUS: {
      if (len != 0) break;
      bool mounted = false;
      bool at_track0 = false;
      bool write_protected = false;
      bool changed = false;
      uint8_t cylinder = 0;
      floppy_stats_t stats;
      error = f12_is_mounted(&fs, &mounted);
      if (error != DISK_OK && error != DISK_ERR_MEDIA_CHANGED) return (disk_result_t){.error = error};
      error = floppy_at_track0(&floppy, &at_track0);
      if (error != DISK_OK) return (disk_result_t){.error = error};
      error = floppy_write_protected(&floppy, &write_protected);
      if (error != DISK_OK) return (disk_result_t){.error = error};
      error = floppy_disk_changed(&floppy, &changed);
      if (error != DISK_OK) return (disk_result_t){.error = error};
      error = floppy_current_track(&floppy, &cylinder);
      if (error != DISK_OK && error != DISK_ERR_NO_TRACK0) return (disk_result_t){.error = error};
      error = floppy_stats(&floppy, &stats);
      if (error != DISK_OK) return (disk_result_t){.error = error};
      int length = snprintf(
          (char *)resp, 512,
          "mounted=%d track=%u track0=%d write_protect=%d disk_change=%d\n"
          "reads=%lu retries=%lu recovered=%lu failed=%lu overruns=%lu",
          mounted ? 1 : 0, cylinder, at_track0 ? 1 : 0, write_protected ? 1 : 0,
          changed ? 1 : 0, (unsigned long)stats.reads, (unsigned long)stats.retries,
          (unsigned long)stats.recovered, (unsigned long)stats.failed,
          (unsigned long)stats.overruns);
      if (length < 0 || length >= 512) return (disk_result_t){.error = DISK_ERR_IO};
      return (disk_result_t){DISK_OK, (size_t)length};
    }

    case RPC_MOTOR:
      if (len != 1 || req[0] > 1) break;
      error = req[0] ? floppy_motor_on(&floppy) : floppy_motor_off(&floppy);
      break;
    case RPC_SELECT:
      if (len != 1 || req[0] > 1) break;
      error = floppy_select(&floppy, req[0] != 0);
      break;
    default:
      return rpc_dispatch(&fs, op, req, len, resp, RPC_MAX_MSG);
  }
  return (disk_result_t){.error = error};
}

int main(void) {
  bool clock_ready = board_clock_init();
  stdio_init_all();
  link_gpio_init();
  if (!clock_ready) {
    printf("[s] clock initialization failed\n");
    for (;;) tight_loop_contents();
  }

  disk_err_t error = floppy_init(&floppy, (floppy_pins_t)FLOPPY_PINS_ALT);
  if (error == DISK_OK) error = f12_init(&fs, floppy_device(&floppy));
  if (error != DISK_OK) {
    printf("[s] initialization failed: %s\n", disk_strerror(error));
    for (;;) tight_loop_contents();
  }
  printf("[s] floppy rpc slave ready on %s\n", BOARD_NAME);

  for (;;) {
    uint8_t hdr[RPC_HDR];
    recv_bytes(hdr, RPC_HDR);
    uint8_t tag = hdr[0];
    uint8_t op = hdr[1];
    uint16_t len = load_le16(hdr + 2);

    if (len > RPC_MAX_MSG) {
      uint8_t sink;
      for (uint32_t i = 0; i < len; i++) recv_bytes(&sink, 1);
      uint8_t rhdr[RPC_HDR] = {tag, (uint8_t)DISK_ERR_INVALID, 0, 0};
      send_bytes(rhdr, RPC_HDR);
      continue;
    }
    recv_bytes(req_buf, len);

    disk_result_t result = dispatch(op, req_buf, len, resp_buf);
    printf("[s] op=%u len=%u -> %s rlen=%lu\n", op, len, disk_strerror(result.error),
           (unsigned long)result.count);

    uint8_t rhdr[RPC_HDR] = {tag, (uint8_t)result.error, 0, 0};
    store_le16(rhdr + 2, (uint16_t)result.count);
    send_bytes(rhdr, RPC_HDR);
    if (result.count) send_bytes(resp_buf, (uint32_t)result.count);
  }
}
