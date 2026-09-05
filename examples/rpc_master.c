#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

#include "disk.h"
#include "rpc_client.h"
#include "spi_link.h"

#define T_FAST 15000u
#define T_MOUNT 60000u
#define T_FSCK 120000u
#define T_FORMAT 300000u

static spi_link_t link;
static uint8_t tag;
static uint32_t polls;
static uint32_t good;
static bool desync;
static uint64_t pending_file;
static uint8_t req_buf[RPC_MAX_MSG];
static uint8_t resp_buf[RPC_MAX_MSG + 1u];
static char write_buf[32768];

static void link_init(void) {
  spi_init(SPI_LINK_MASTER_SPI, SPI_LINK_HZ);
  spi_set_format(SPI_LINK_MASTER_SPI, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
  gpio_set_function(SPI_LINK_MASTER_SCK, GPIO_FUNC_SPI);
  gpio_set_function(SPI_LINK_MASTER_TX, GPIO_FUNC_SPI);
  gpio_set_function(SPI_LINK_MASTER_RX, GPIO_FUNC_SPI);
  gpio_init(SPI_LINK_MASTER_CS);
  gpio_set_dir(SPI_LINK_MASTER_CS, GPIO_OUT);
  gpio_put(SPI_LINK_MASTER_CS, 1);
}

static void xfer(void) {
  uint8_t tx[SPI_LINK_FRAME];
  uint8_t wtx[SPI_LINK_WIRE];
  uint8_t wrx[SPI_LINK_WIRE];
  spi_link_build(&link, tx);
  memcpy(wtx, tx, SPI_LINK_FRAME);
  wtx[SPI_LINK_FRAME] = 0;

  gpio_put(SPI_LINK_MASTER_CS, 0);
  spi_write_read_blocking(SPI_LINK_MASTER_SPI, wtx, wrx, SPI_LINK_WIRE);
  gpio_put(SPI_LINK_MASTER_CS, 1);
  polls++;

  const uint8_t *frame = NULL;
  if (wrx[0] == SPI_LINK_MAGIC) frame = wrx;
  else if (wrx[0] == 0 && wrx[1] == SPI_LINK_MAGIC) frame = wrx + 1;
  if (frame && spi_link_frame_ok(frame)) {
    good++;
    spi_link_accept(&link, frame);
  }
  sleep_us(300);
}

static bool expired(absolute_time_t deadline) {
  return absolute_time_diff_us(get_absolute_time(), deadline) < 0;
}

static bool send_bytes(const uint8_t *data, uint32_t count, uint32_t timeout_ms) {
  absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
  while (count) {
    while (link.pending_len) {
      if (expired(deadline)) return false;
      xfer();
    }
    uint8_t chunk = spi_link_stage(&link, data, count);
    data += chunk;
    count -= chunk;
  }
  while (link.pending_len) {
    if (expired(deadline)) return false;
    xfer();
  }
  return true;
}

static bool recv_bytes(uint8_t *data, uint32_t count, uint32_t timeout_ms) {
  absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
  uint32_t got = 0;
  for (;;) {
    got += spi_link_drain(&link, data + got, count - got);
    if (got >= count) return true;
    if (expired(deadline)) return false;
    xfer();
  }
}

static void resync(void) {
  absolute_time_t deadline = make_timeout_time_ms(300);
  while (!expired(deadline)) xfer();
  link.queue_read = link.queue_write;
  desync = false;
}

static bool rpc_call(uint8_t op, const uint8_t *req, uint16_t req_len,
                     disk_err_t *status, uint8_t *resp, uint32_t *resp_len,
                     uint32_t timeout_ms) {
  if (desync) resync();
  link.queue_read = link.queue_write;
  tag++;

  uint8_t hdr[RPC_HDR] = {tag, op, 0, 0};
  store_le16(hdr + 2, req_len);
  if (!send_bytes(hdr, RPC_HDR, T_FAST) ||
      (req_len && !send_bytes(req, req_len, T_FAST))) {
    desync = true;
    return false;
  }

  for (;;) {
    uint8_t rhdr[RPC_HDR];
    if (!recv_bytes(rhdr, RPC_HDR, timeout_ms)) break;
    uint16_t rlen = load_le16(rhdr + 2);
    if (rlen > RPC_MAX_MSG || !recv_bytes(resp, rlen, timeout_ms)) break;
    if (rhdr[0] != tag) continue;
    if (rhdr[1] > DISK_ERR_LAST) break;
    *status = (disk_err_t)rhdr[1];
    *resp_len = rlen;
    return true;
  }
  desync = true;
  return false;
}

static disk_result_t file_exchange(void *ctx, uint8_t op,
                                   const uint8_t *request, size_t length,
                                   uint8_t *response, size_t capacity) {
  (void)ctx;
  disk_err_t status;
  uint32_t received;
  if (!rpc_call(op, request, (uint16_t)length, &status, resp_buf, &received,
                T_MOUNT)) {
    return (disk_result_t){.error = DISK_ERR_TIMEOUT};
  }
  if (received > capacity) return (disk_result_t){.error = DISK_ERR_CORRUPT};
  if (received != 0) memcpy(response, resp_buf, received);
  return (disk_result_t){status, received};
}

static rpc_client_t client = {.exchange = file_exchange};

static int call_simple(uint8_t op, const uint8_t *req, uint16_t req_len,
                       uint32_t timeout_ms) {
  disk_err_t status;
  uint32_t rlen;
  if (!rpc_call(op, req, req_len, &status, resp_buf, &rlen, timeout_ms)) {
    printf("error: link down\n");
    return -1;
  }
  if (status != DISK_OK) {
    printf("error: %s\n", disk_strerror(status));
    return -1;
  }
  resp_buf[rlen] = 0;
  return (int)rlen;
}

static int readline(char *buf, size_t max) {
  size_t n = 0;
  for (;;) {
    int c = getchar();
    if (c == '\r' || c == '\n') {
      printf("\n");
      buf[n] = 0;
      return (int)n;
    }
    if (c == 0x7F || c == 0x08) {
      if (n > 0) {
        n--;
        printf("\b \b");
      }
      continue;
    }
    if (c >= 32 && c < 127 && n + 1u < max) {
      buf[n++] = (char)c;
      putchar(c);
    }
  }
}

static int tokenize(char *line, char **argv, int max) {
  int argc = 0;
  char *p = line;
  while (*p && argc < max) {
    while (*p == ' ') *p++ = 0;
    if (!*p) break;
    argv[argc++] = p;
    while (*p && *p != ' ') p++;
  }
  return argc;
}

static uint16_t path_request(uint8_t *out, const char *path) {
  size_t n = strlen(path) + 1u;
  memcpy(out, path, n);
  return (uint16_t)n;
}

static bool file_open(const char *path, f12_open_mode_t mode) {
  if (pending_file != 0) {
    printf("a file is still open; use close or abort first\n");
    return false;
  }
  disk_err_t error = rpc_file_open(&client, path, mode, &pending_file);
  if (error != DISK_OK) printf("error: %s\n", disk_strerror(error));
  return error == DISK_OK;
}

static bool file_finish(bool abort) {
  if (pending_file == 0) return true;
  disk_err_t error = DISK_OK;
  for (unsigned attempt = 0; attempt < 3; attempt++) {
    error = abort ? rpc_file_abort(&client, pending_file)
                  : rpc_file_close(&client, pending_file);
    if (!disk_err_is_io(error) || error == DISK_ERR_MEDIA_CHANGED) break;
  }
  if (error == DISK_OK || error == DISK_ERR_BAD_HANDLE ||
      error == DISK_ERR_MEDIA_CHANGED || error == DISK_ERR_NOT_MOUNTED ||
      error == DISK_ERR_NOT_INITIALIZED) {
    pending_file = 0;
  }
  if (error != DISK_OK) {
    printf("error: %s\n", disk_strerror(error));
    if (pending_file != 0) printf("file retained; use %s to retry\n", abort ? "abort" : "close");
  }
  return error == DISK_OK;
}

static void hexdump_row(uint32_t offset, const uint8_t *data, int count) {
  printf("%08lx  ", (unsigned long)offset);
  for (int j = 0; j < 16; j++) {
    if (j < count) printf("%02x ", data[j]);
    else printf("   ");
  }
  printf(" ");
  for (int j = 0; j < count; j++) {
    uint8_t c = data[j];
    putchar(c >= 32 && c < 127 ? c : '.');
  }
  printf("\n");
}

static void cmd_cat(const char *path, bool hex) {
  if (!file_open(path, F12_OPEN_READ)) return;
  uint8_t data[RPC_CHUNK];
  uint32_t offset = 0;
  for (;;) {
    disk_result_t result = rpc_file_read(&client, pending_file, data, sizeof(data));
    int count = (int)result.count;
    if (!hex) {
      for (int i = 0; i < count; i++) putchar(data[i]);
    } else {
      for (int i = 0; i < count; i += 16) {
        hexdump_row(offset + (uint32_t)i, data + i,
                    count - i < 16 ? count - i : 16);
      }
    }
    offset += (uint32_t)result.count;
    if (result.error != DISK_OK && result.error != DISK_END) {
      printf("\nerror after %lu bytes: %s\n", (unsigned long)offset,
             disk_strerror(result.error));
    }
    if (result.error != DISK_OK || result.count == 0) break;
  }
  if (!hex) printf("\n");
  file_finish(false);
}

static void cmd_write(const char *path) {
  if (pending_file != 0) {
    printf("a file is still open; use close or abort first\n");
    return;
  }
  printf("enter text, end with '.' on its own line\n");
  uint32_t n = 0;
  char line[256];
  for (;;) {
    readline(line, sizeof(line));
    if (line[0] == '.' && line[1] == 0) break;
    uint32_t l = (uint32_t)strlen(line);
    if (n + l + 1u >= sizeof(write_buf)) {
      printf("buffer full\n");
      break;
    }
    memcpy(write_buf + n, line, l);
    n += l;
    write_buf[n++] = '\n';
  }

  if (!file_open(path, F12_OPEN_WRITE)) return;
  uint32_t offset = 0;
  while (offset < n) {
    size_t chunk = n - offset > RPC_CHUNK ? RPC_CHUNK : n - offset;
    disk_result_t result = rpc_file_write(&client, pending_file,
                                          write_buf + offset, chunk);
    offset += (uint32_t)result.count;
    if (result.error != DISK_OK) {
      printf("error after %lu bytes: %s\n", (unsigned long)offset,
             disk_strerror(result.error));
      file_finish(true);
      return;
    }
  }
  if (file_finish(false)) printf("wrote %lu bytes\n", (unsigned long)n);
}

static void print_stat(const uint8_t *entry) {
  f12_stat_t stat;
  rpc_decode_stat(entry, &stat);
  printf("  %-13s %8lu  attr=%02x%s\n", stat.name, (unsigned long)stat.size,
         stat.attr, (stat.attr & FAT12_ATTR_DIRECTORY) != 0 ? "  <dir>" : "");
}

static void cmd_ls(void) {
  int r = call_simple(RPC_LIST, NULL, 0, T_MOUNT);
  if (r < 0) return;
  int count = 0;
  uint32_t total = 0;
  for (int offset = 0; offset + (int)RPC_STAT_SIZE <= r; offset += (int)RPC_STAT_SIZE) {
    print_stat(resp_buf + offset);
    total += load_le32(resp_buf + offset);
    count++;
  }
  printf("%d files, %lu bytes\n", count, (unsigned long)total);
}

static void cmd_fsck(bool fix) {
  uint8_t flag = fix ? 1 : 0;
  int length = call_simple(RPC_FSCK, &flag, 1, T_FSCK);
  if (length < 0) return;
  if (length != RPC_FSCK_SIZE) {
    printf("error: invalid fsck response\n");
    return;
  }
  fat12_fsck_t report;
  rpc_decode_fsck(resp_buf, &report);
  printf("files=%u dirs=%u lost=%u broken=%u crosslinked=%u loops=%u "
         "fat_mismatch=%u freed=%u\n",
         report.files, report.directories, report.lost_clusters,
         report.broken_chains, report.crosslinked, report.loops,
         report.fat_mismatch, report.freed);
  if (fix) printf("repair verified clean\n");
  else printf("%s\n", fat12_fsck_clean(&report) ? "clean" : "DIRTY -- run 'fsck fix' to repair");
}

static void cmd_help(void) {
  printf("  ping                  probe slave\n");
  printf("  mount / unmount       mount floppy filesystem\n");
  printf("  ls                    list files\n");
  printf("  cat <file>            print file\n");
  printf("  hexdump <file>        hex dump file\n");
  printf("  write <file>          write file (end with . line)\n");
  printf("  close                 retry closing the pending file\n");
  printf("  abort                 discard the pending write\n");
  printf("  rm <file>             delete file\n");
  printf("  mv <from> <to>        rename file\n");
  printf("  stat <file>           file info\n");
  printf("  format [label] [full] format disk\n");
  printf("  fsck [fix]            check filesystem\n");
  printf("  status                drive status\n");
  printf("  motor on|off          spindle motor\n");
  printf("  select on|off         drive select\n");
  printf("  link                  link statistics\n");
}

int main(void) {
  stdio_init_all();
  link_init();

  printf("\n=== floppy rpc master ===\n");

  char line[256];
  char *argv[4];

  for (;;) {
    printf("m> ");
    readline(line, sizeof(line));
    int argc = tokenize(line, argv, 4);
    if (argc == 0) continue;

    if (!strcmp(argv[0], "help") || !strcmp(argv[0], "?")) {
      cmd_help();
    } else if (!strcmp(argv[0], "ping")) {
      if (call_simple(RPC_PING, NULL, 0, 2000) >= 0) printf("%s\n", (const char *)resp_buf);
    } else if (!strcmp(argv[0], "mount")) {
      if (call_simple(RPC_MOUNT, NULL, 0, T_MOUNT) >= 0) {
        printf("mounted\n");
        cmd_fsck(false);
      }
    } else if (!strcmp(argv[0], "unmount") || !strcmp(argv[0], "umount")) {
      if (call_simple(RPC_UNMOUNT, NULL, 0, T_MOUNT) >= 0) printf("unmounted\n");
    } else if (!strcmp(argv[0], "ls") || !strcmp(argv[0], "dir")) {
      cmd_ls();
    } else if (!strcmp(argv[0], "cat") && argc >= 2) {
      cmd_cat(argv[1], false);
    } else if (!strcmp(argv[0], "hexdump") && argc >= 2) {
      cmd_cat(argv[1], true);
    } else if (!strcmp(argv[0], "write") && argc >= 2) {
      cmd_write(argv[1]);
    } else if (!strcmp(argv[0], "close")) {
      if (file_finish(false)) printf("closed\n");
    } else if (!strcmp(argv[0], "abort")) {
      if (file_finish(true)) printf("aborted\n");
    } else if (!strcmp(argv[0], "rm") && argc >= 2) {
      uint16_t n = path_request(req_buf, argv[1]);
      if (call_simple(RPC_DELETE, req_buf, n, T_MOUNT) >= 0) printf("deleted\n");
    } else if (!strcmp(argv[0], "mv") && argc >= 3) {
      uint16_t a = path_request(req_buf, argv[1]);
      uint16_t b = path_request(req_buf + a, argv[2]);
      if (call_simple(RPC_RENAME, req_buf, (uint16_t)(a + b), T_MOUNT) >= 0) {
        printf("renamed\n");
      }
    } else if (!strcmp(argv[0], "stat") && argc >= 2) {
      uint16_t n = path_request(req_buf, argv[1]);
      if (call_simple(RPC_STAT, req_buf, n, T_MOUNT) >= (int)RPC_STAT_SIZE) {
        print_stat(resp_buf);
      }
    } else if (!strcmp(argv[0], "format")) {
      const char *label = "PICODISK";
      bool full = false;
      if (argc >= 2) {
        if (!strcmp(argv[argc - 1], "full")) {
          full = true;
          if (argc >= 3) label = argv[1];
        } else {
          label = argv[1];
        }
      }
      printf("format as \"%s\" (%s)? [y/N] ", label, full ? "full" : "quick");
      readline(line, sizeof(line));
      if (line[0] != 'y' && line[0] != 'Y') {
        printf("cancelled\n");
        continue;
      }
      req_buf[0] = full ? 1 : 0;
      uint16_t n = path_request(&req_buf[1], label);
      if (call_simple(RPC_FORMAT, req_buf, (uint16_t)(n + 1u), T_FORMAT) >= 0) {
        printf("formatted\n");
      }
    } else if (!strcmp(argv[0], "fsck")) {
      cmd_fsck(argc >= 2 && !strcmp(argv[1], "fix"));
    } else if (!strcmp(argv[0], "status")) {
      if (call_simple(RPC_STATUS, NULL, 0, T_FAST) >= 0) printf("%s\n", (const char *)resp_buf);
    } else if (!strcmp(argv[0], "motor") && argc >= 2) {
      uint8_t on = !strcmp(argv[1], "on");
      call_simple(RPC_MOTOR, &on, 1, T_MOUNT);
    } else if (!strcmp(argv[0], "select") && argc >= 2) {
      uint8_t on = !strcmp(argv[1], "on");
      call_simple(RPC_SELECT, &on, 1, T_FAST);
    } else if (!strcmp(argv[0], "link")) {
      printf("polls=%lu good=%lu%s\n", (unsigned long)polls, (unsigned long)good,
             desync ? " (desync)" : "");
    } else {
      printf("unknown command, try 'help'\n");
    }
  }
}
