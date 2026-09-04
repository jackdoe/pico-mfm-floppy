#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

#include "board.h"
#include "f12.h"
#include "floppy.h"
#include "rpc.h"
#include "spi_link.h"

static floppy_t floppy;
static f12_t fs;
static f12_file_t handles[F12_MAX_OPEN_FILES];
static bool handle_used[F12_MAX_OPEN_FILES];
static spi_link_t link;
static uint8_t req_buf[RPC_MAX_MSG + 1u];
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

static f12_file_t *handle_get(uint8_t handle) {
  return handle < F12_MAX_OPEN_FILES && handle_used[handle] ? &handles[handle] : NULL;
}

static bool handle_gone(disk_err_t error) {
  return error == DISK_OK || error == DISK_ERR_BAD_HANDLE ||
         error == DISK_ERR_MEDIA_CHANGED || error == DISK_ERR_NOT_MOUNTED ||
         error == DISK_ERR_NOT_INITIALIZED;
}

static void encode_stat(uint8_t *out, const f12_stat_t *stat) {
  rpc_store32(out, stat->size);
  out[4] = stat->attr;
  out[5] = (stat->attr & FAT12_ATTR_DIRECTORY) != 0;
  memcpy(out + 6, stat->name, sizeof(stat->name));
}

typedef struct {
  uint8_t *out;
  uint32_t count;
} list_ctx_t;

static disk_err_t list_entry(void *ctx, const f12_stat_t *stat) {
  list_ctx_t *list = ctx;
  if (list->count + RPC_STAT_SIZE > RPC_MAX_MSG) return DISK_ERR_FULL;
  encode_stat(list->out + list->count, stat);
  list->count += RPC_STAT_SIZE;
  return DISK_OK;
}

static disk_err_t unmount_if_mounted(void) {
  bool mounted;
  disk_err_t error = f12_is_mounted(&fs, &mounted);
  if (error != DISK_OK) return error;
  return mounted ? f12_unmount(&fs) : DISK_OK;
}

static disk_err_t dispatch(uint8_t op, uint8_t *req, uint16_t len,
                           uint8_t *resp, uint32_t *rlen) {
  *rlen = 0;
  switch (op) {
    case RPC_PING:
      *rlen = (uint32_t)snprintf((char *)resp, 64, "pico-floppy rpc %s %s",
                                 __DATE__, __TIME__);
      return DISK_OK;

    case RPC_MOUNT: {
      disk_err_t error = unmount_if_mounted();
      return error != DISK_OK ? error : f12_mount(&fs);
    }

    case RPC_UNMOUNT:
      return f12_unmount(&fs);

    case RPC_FORMAT: {
      if (len < 2) return DISK_ERR_INVALID;
      disk_err_t error = unmount_if_mounted();
      if (error != DISK_OK) return error;
      return f12_format(&fs, (f12_format_options_t){
                                 .label = (const char *)req + 1,
                                 .mode = req[0] ? F12_FORMAT_FULL : F12_FORMAT_QUICK,
                             });
    }

    case RPC_FSCK: {
      if (len < 1) return DISK_ERR_INVALID;
      fat12_fsck_t report;
      disk_err_t error = f12_fsck(&fs, &report, req[0] != 0);
      if (error != DISK_OK) return error;
      resp[0] = fat12_fsck_clean(&report) ? 0 : 1;
      *rlen = 1u + (uint32_t)snprintf(
          (char *)resp + 1, 512,
          "files=%u dirs=%u lost=%u broken=%u crosslinked=%u loops=%u "
          "fat_mismatch=%u freed=%u",
          report.files, report.directories, report.lost_clusters,
          report.broken_chains, report.crosslinked, report.loops,
          report.fat_mismatch, report.freed);
      return DISK_OK;
    }

    case RPC_OPEN: {
      if (len < 3) return DISK_ERR_INVALID;
      for (uint8_t handle = 0; handle < F12_MAX_OPEN_FILES; handle++) {
        if (handle_used[handle]) continue;
        disk_err_t error = f12_open(&fs, (const char *)req + 1,
                                    req[0] == 'w' ? F12_OPEN_WRITE : F12_OPEN_READ,
                                    &handles[handle]);
        if (error != DISK_OK) return error;
        handle_used[handle] = true;
        resp[0] = handle;
        *rlen = 1;
        return DISK_OK;
      }
      return DISK_ERR_TOO_MANY;
    }

    case RPC_CLOSE: {
      if (len < 1) return DISK_ERR_INVALID;
      f12_file_t *file = handle_get(req[0]);
      if (!file) return DISK_ERR_BAD_HANDLE;
      disk_err_t error = f12_close(file);
      if (handle_gone(error)) handle_used[req[0]] = false;
      return error;
    }

    case RPC_READ: {
      if (len < 3) return DISK_ERR_INVALID;
      f12_file_t *file = handle_get(req[0]);
      if (!file) return DISK_ERR_BAD_HANDLE;
      uint16_t want = rpc_load16(req + 1);
      if (want > RPC_CHUNK) want = RPC_CHUNK;
      disk_result_t result = f12_read(file, resp, want);
      *rlen = (uint32_t)result.count;
      return result.error == DISK_END ? DISK_OK : result.error;
    }

    case RPC_WRITE: {
      if (len < 1) return DISK_ERR_INVALID;
      f12_file_t *file = handle_get(req[0]);
      if (!file) return DISK_ERR_BAD_HANDLE;
      uint32_t size = (uint32_t)len - 1u;
      uint32_t total = 0;
      disk_err_t error = DISK_OK;
      while (total < size && error == DISK_OK) {
        disk_result_t result = f12_write(file, req + 1 + total, size - total);
        total += (uint32_t)result.count;
        error = result.error;
      }
      rpc_store16(resp, (uint16_t)total);
      *rlen = 2;
      return error;
    }

    case RPC_SEEK: {
      if (len < 5) return DISK_ERR_INVALID;
      f12_file_t *file = handle_get(req[0]);
      if (!file) return DISK_ERR_BAD_HANDLE;
      return f12_seek(file, rpc_load32(req + 1));
    }

    case RPC_STAT: {
      if (len < 2) return DISK_ERR_INVALID;
      f12_stat_t stat;
      disk_err_t error = f12_stat(&fs, (const char *)req, &stat);
      if (error != DISK_OK) return error;
      encode_stat(resp, &stat);
      *rlen = RPC_STAT_SIZE;
      return DISK_OK;
    }

    case RPC_DELETE:
      if (len < 2) return DISK_ERR_INVALID;
      return f12_delete(&fs, (const char *)req);

    case RPC_RENAME: {
      const char *from = (const char *)req;
      uint32_t from_len = 0;
      while (from_len < len && from[from_len] != 0) from_len++;
      if (from_len + 2u > len) return DISK_ERR_INVALID;
      return f12_rename(&fs, from, from + from_len + 1u);
    }

    case RPC_LIST: {
      list_ctx_t list = {.out = resp, .count = 0};
      disk_err_t error = f12_list(&fs, list_entry, &list);
      if (error != DISK_OK) return error;
      *rlen = list.count;
      return DISK_OK;
    }

    case RPC_STATUS: {
      bool mounted = false;
      bool at_track0 = false;
      bool write_protected = false;
      bool changed = false;
      uint8_t cylinder = 0;
      floppy_stats_t stats;
      disk_err_t error = f12_is_mounted(&fs, &mounted);
      if (error != DISK_OK && error != DISK_ERR_MEDIA_CHANGED) return error;
      error = floppy_at_track0(&floppy, &at_track0);
      if (error != DISK_OK) return error;
      error = floppy_write_protected(&floppy, &write_protected);
      if (error != DISK_OK) return error;
      error = floppy_disk_changed(&floppy, &changed);
      if (error != DISK_OK) return error;
      error = floppy_current_track(&floppy, &cylinder);
      if (error != DISK_OK && error != DISK_ERR_NO_TRACK0) return error;
      error = floppy_stats(&floppy, &stats);
      if (error != DISK_OK) return error;
      *rlen = (uint32_t)snprintf(
          (char *)resp, 512,
          "mounted=%d track=%u track0=%d write_protect=%d disk_change=%d\n"
          "reads=%lu retries=%lu recovered=%lu failed=%lu overruns=%lu",
          mounted ? 1 : 0, cylinder, at_track0 ? 1 : 0, write_protected ? 1 : 0,
          changed ? 1 : 0, (unsigned long)stats.reads, (unsigned long)stats.retries,
          (unsigned long)stats.recovered, (unsigned long)stats.failed,
          (unsigned long)stats.overruns);
      return DISK_OK;
    }

    case RPC_MOTOR:
      if (len < 1) return DISK_ERR_INVALID;
      return req[0] ? floppy_motor_on(&floppy) : floppy_motor_off(&floppy);

    case RPC_SELECT:
      if (len < 1) return DISK_ERR_INVALID;
      return floppy_select(&floppy, req[0] != 0);

    default:
      return DISK_ERR_INVALID;
  }
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
    uint16_t len = rpc_load16(hdr + 2);

    if (len > RPC_MAX_MSG) {
      uint8_t sink;
      for (uint32_t i = 0; i < len; i++) recv_bytes(&sink, 1);
      uint8_t rhdr[RPC_HDR] = {tag, (uint8_t)DISK_ERR_INVALID, 0, 0};
      send_bytes(rhdr, RPC_HDR);
      continue;
    }
    recv_bytes(req_buf, len);
    req_buf[len] = 0;

    uint32_t rlen = 0;
    disk_err_t status = dispatch(op, req_buf, len, resp_buf, &rlen);
    printf("[s] op=%u len=%u -> %s rlen=%lu\n", op, len, disk_strerror(status),
           (unsigned long)rlen);

    uint8_t rhdr[RPC_HDR] = {tag, (uint8_t)status, 0, 0};
    rpc_store16(rhdr + 2, (uint16_t)rlen);
    send_bytes(rhdr, RPC_HDR);
    if (rlen) send_bytes(resp_buf, rlen);
  }
}
