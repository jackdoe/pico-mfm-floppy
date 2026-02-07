#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "floppy.h"
#include "f12.h"
#include "mfm_decode.h"

// ============== Configuration ==============

static floppy_t floppy;
static f12_t fs;
static bool mounted;

// ============== Buffers ==============

#define CMD_BUF_SIZE  256
#define IO_BUF_SIZE   512
#define SELF_BUF_SIZE 8192
#define MAX_ARGS      4
#define PULSE_BINS    128

static char cmd_buf[CMD_BUF_SIZE];
static uint8_t io_buf[IO_BUF_SIZE];
static uint8_t self_buf[SELF_BUF_SIZE];

// ============== Forward Declarations ==============

typedef void (*cmd_fn_t)(int argc, char **argv);

typedef struct {
  const char *name;
  const char *alias;
  cmd_fn_t fn;
  bool needs_mount;
  const char *usage;
  const char *desc;
} cmd_entry_t;

static void cmd_help(int argc, char **argv);
static void cmd_ls(int argc, char **argv);
static void cmd_cat(int argc, char **argv);
static void cmd_hexdump(int argc, char **argv);
static void cmd_write(int argc, char **argv);
static void cmd_rm(int argc, char **argv);
static void cmd_cp(int argc, char **argv);
static void cmd_mv(int argc, char **argv);
static void cmd_stat(int argc, char **argv);
static void cmd_format(int argc, char **argv);
static void cmd_mount(int argc, char **argv);
static void cmd_unmount(int argc, char **argv);
static void cmd_status(int argc, char **argv);
static void cmd_motor(int argc, char **argv);
static void cmd_select(int argc, char **argv);
static void cmd_home(int argc, char **argv);
static void cmd_pins(int argc, char **argv);
static void cmd_poll(int argc, char **argv);
static void cmd_flux(int argc, char **argv);
static void cmd_seek(int argc, char **argv);
static void cmd_dump(int argc, char **argv);
static void cmd_mfm(int argc, char **argv);
static void cmd_selftest(int argc, char **argv);
static void cmd_reboot(int argc, char **argv);

// ============== Command Table ==============

static const cmd_entry_t commands[] = {
  {"help",    "?",     cmd_help,    false, "help",                "Show all commands"},
  {"ls",      "dir",   cmd_ls,      true,  "ls",                  "List files"},
  {"cat",     "read",  cmd_cat,     true,  "cat <file>",          "Print file contents"},
  {"hexdump", "xxd",   cmd_hexdump, true,  "hexdump <file>",      "Hex dump file contents"},
  {"write",   NULL,    cmd_write,   true,  "write <file>",        "Write file (end with . on own line)"},
  {"rm",      "del",   cmd_rm,      true,  "rm <file>",           "Delete file"},
  {"cp",      NULL,    cmd_cp,      true,  "cp <src> <dst>",      "Copy file"},
  {"mv",      NULL,    cmd_mv,      true,  "mv <src> <dst>",      "Move/rename file"},
  {"stat",    NULL,    cmd_stat,    true,  "stat <file>",         "File details and cluster chain"},
  {"format",  NULL,    cmd_format,  false, "format [label] [full]","Format disk"},
  {"mount",   NULL,    cmd_mount,   false, "mount",               "Mount filesystem"},
  {"unmount", "umount",cmd_unmount, false, "unmount",             "Unmount filesystem"},
  {"status",  "info",  cmd_status,  false, "status",              "Drive status and disk info"},
  {"motor",   NULL,    cmd_motor,   false, "motor [on|off]",      "Control motor"},
  {"select",  "sel",   cmd_select,  false, "select [on|off]",     "Control drive select"},
  {"home",    NULL,    cmd_home,    false, "home",                "Seek to track 0"},
  {"pins",    "gpio",  cmd_pins,    false, "pins",                "Read all GPIO pin states"},
  {"poll",    NULL,    cmd_poll,    false, "poll",                "Poll read_data + index (no PIO)"},
  {"flux",    NULL,    cmd_flux,    false, "flux [count]",        "Dump raw flux transitions"},
  {"seek",    NULL,    cmd_seek,    false, "seek <track>",        "Seek head to track (0-79)"},
  {"dump",    NULL,    cmd_dump,    false, "dump <trk> <side> [sector]", "Raw sector hex dump"},
  {"mfm",     NULL,    cmd_mfm,    false, "mfm <track> <side>",  "MFM signal analysis"},
  {"selftest",NULL,    cmd_selftest,false, "selftest",            "Format + write/read/verify cycle"},
  {"reboot",  NULL,    cmd_reboot,  false, "reboot",              "Reboot the Pico"},
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

// ============== IO Helpers ==============

static f12_err_t do_mount(void) {
  f12_io_t io = {
    .read = floppy_io_read,
    .write = floppy_io_write,
    .disk_changed = floppy_io_disk_changed,
    .write_protected = floppy_io_write_protected,
    .ctx = &floppy,
  };
  return f12_mount(&fs, io);
}

static void setup_io(void) {
  fs.io = (f12_io_t){
    .read = floppy_io_read,
    .write = floppy_io_write,
    .disk_changed = floppy_io_disk_changed,
    .write_protected = floppy_io_write_protected,
    .ctx = &floppy,
  };
}

static void f12_write_full(f12_file_t *f, const void *buf, uint32_t len) {
  uint32_t written = 0;
  while (written < len) {
    uint32_t chunk = len - written;
    if (chunk > 512) chunk = 512;
    int n = f12_write(f, (const uint8_t *)buf + written, chunk);
    if (n <= 0) break;
    written += n;
  }
}

static uint32_t f12_read_full(f12_file_t *f, void *buf, uint32_t max_len) {
  uint32_t total = 0;
  int n;
  while ((n = f12_read(f, (uint8_t *)buf + total, 512)) > 0) {
    total += n;
    if (total >= max_len) break;
  }
  return total;
}

static uint16_t count_free_clusters(void) {
  uint16_t free_clusters = 0;
  for (uint16_t c = 2; c < fs.fat.total_clusters + 2; c++) {
    uint16_t next;
    if (fat12_get_entry(&fs.fat, c, &next) == FAT12_OK && next == 0) {
      free_clusters++;
    }
  }
  return free_clusters;
}

static void upcase(char *s) {
  while (*s) {
    *s = toupper((unsigned char)*s);
    s++;
  }
}

// ============== Line Editor ==============

static void print_prompt(void) {
  if (mounted)
    printf("[A:]> ");
  else
    printf("[--]> ");
}

static int cli_readline(char *buf, int max) {
  int pos = 0;
  memset(buf, 0, max);

  for (;;) {
    int c = getchar();
    if (c == EOF) {
      tight_loop_contents();
      continue;
    }

    if (c == '\r' || c == '\n') {
      printf("\r\n");
      buf[pos] = '\0';
      return pos;
    }

    // Ctrl-C: cancel line
    if (c == 3) {
      printf("^C\r\n");
      buf[0] = '\0';
      return 0;
    }

    // Ctrl-U: clear line
    if (c == 21) {
      while (pos > 0) {
        printf("\b \b");
        pos--;
      }
      continue;
    }

    // Backspace (BS or DEL)
    if (c == 8 || c == 127) {
      if (pos > 0) {
        printf("\b \b");
        pos--;
      }
      continue;
    }

    // Printable character
    if (pos < max - 1 && c >= 32 && c < 127) {
      buf[pos++] = c;
      putchar(c);
    }
  }
}

// ============== Tokenizer ==============

static int tokenize(char *buf, char **argv, int max_args) {
  int argc = 0;
  char *p = buf;

  while (*p && argc < max_args) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') break;
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = '\0';
  }
  return argc;
}

// ============== Dispatch ==============

static const cmd_entry_t *find_command(const char *name) {
  for (unsigned i = 0; i < NUM_COMMANDS; i++) {
    if (strcasecmp(name, commands[i].name) == 0)
      return &commands[i];
    if (commands[i].alias && strcasecmp(name, commands[i].alias) == 0)
      return &commands[i];
  }
  return NULL;
}

// ============== MFM Helpers (ported from mfmstats.c) ==============

typedef struct {
  uint32_t short_count;
  uint32_t medium_count;
  uint32_t long_count;
  uint32_t invalid_count;
  uint32_t total_pulses;
  uint32_t histogram[PULSE_BINS];
  uint16_t T2_max;
  uint16_t T3_max;
  uint32_t syncs;
  uint32_t sectors;
  uint32_t crc_errors;
} track_stats_t;

static inline bool flux_data_available(void) {
  return floppy.read.half || !pio_sm_is_rx_fifo_empty(floppy.read.pio, floppy.read.sm);
}

static inline uint16_t flux_read_raw(void) {
  if (floppy.read.half) {
    uint16_t v = floppy.read.half;
    floppy.read.half = 0;
    return v;
  }
  uint32_t pv = pio_sm_get_blocking(floppy.read.pio, floppy.read.sm);
  floppy.read.half = pv >> 16;
  return pv & 0xffff;
}

static inline uint16_t flux_read_wait(void) {
  while (!flux_data_available()) {
    tight_loop_contents();
  }
  return flux_read_raw();
}

static void gpio_put_oc(uint pin, bool value) {
  if (value == 0) {
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_OUT);
  } else {
    gpio_set_dir(pin, GPIO_IN);
  }
}

static void read_track_stats(int track, int side, track_stats_t *stats) {
  memset(stats, 0, sizeof(*stats));

  floppy_seek(&floppy, track);
  gpio_put_oc(floppy.pins.side_select, side == 0 ? 1 : 0);

  pio_sm_exec(floppy.read.pio, floppy.read.sm, pio_encode_jmp(floppy.read.offset));
  pio_sm_restart(floppy.read.pio, floppy.read.sm);
  pio_sm_clear_fifos(floppy.read.pio, floppy.read.sm);
  floppy.read.half = 0;
  pio_sm_set_enabled(floppy.read.pio, floppy.read.sm, true);

  mfm_t mfm;
  mfm_init(&mfm);
  mfm_reset(&mfm);

  uint16_t prev = flux_read_wait() >> 1;
  bool ix_prev = false;
  sector_t sector;
  int ix_edges = 0;

  while (ix_edges < 6) {
    uint16_t value = flux_read_wait();
    uint8_t ix = value & 1;
    uint16_t cnt = value >> 1;
    int delta = prev - cnt;
    if (delta < 0) delta += 0x8000;

    if (ix != ix_prev) ix_edges++;
    ix_prev = ix;

    if (delta > 0 && delta < PULSE_BINS) {
      stats->histogram[delta]++;
    }
    stats->total_pulses++;

    if (delta < MFM_PULSE_FLOOR || delta >= MFM_PULSE_CEILING) {
      stats->invalid_count++;
    } else if (delta <= mfm.T2_max) {
      stats->short_count++;
    } else if (delta <= mfm.T3_max) {
      stats->medium_count++;
    } else {
      stats->long_count++;
    }

    mfm_feed(&mfm, delta, &sector);
    prev = cnt;
  }

  pio_sm_set_enabled(floppy.read.pio, floppy.read.sm, false);

  stats->T2_max = mfm.T2_max;
  stats->T3_max = mfm.T3_max;
  stats->syncs = mfm.syncs_found;
  stats->sectors = mfm.sectors_read;
  stats->crc_errors = mfm.crc_errors;
}

static void print_histogram(track_stats_t *stats) {
  uint32_t peak = 0;
  for (int i = 0; i < PULSE_BINS; i++) {
    if (stats->histogram[i] > peak) peak = stats->histogram[i];
  }
  if (peak == 0) return;

  int first = 0, last = PULSE_BINS - 1;
  while (first < PULSE_BINS && stats->histogram[first] == 0) first++;
  while (last > first && stats->histogram[last] == 0) last--;

  printf("  Pulse Distribution (delta ticks):\n");
  for (int i = first; i <= last; i++) {
    if (stats->histogram[i] == 0) continue;
    int bar = (stats->histogram[i] * 50) / peak;
    printf("  %3d: %6lu |", i, stats->histogram[i]);
    for (int j = 0; j < bar; j++) printf("#");
    printf("\n");
  }
}

// ============== Selftest Helpers (ported from selftest.c) ==============

static uint32_t gen_pattern_byte(int file_id, uint32_t offset) {
  uint32_t v = file_id * 2654435761u + offset * 40503u;
  return (v >> 16) & 0xFF;
}

static uint32_t checksum_buf(const uint8_t *buf, size_t len) {
  uint32_t sum = 0;
  for (size_t i = 0; i < len; i++) {
    sum = (sum << 5) + sum + buf[i];
  }
  return sum;
}

static uint32_t pattern_checksum(int file_id, uint32_t size) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < size; i++) {
    sum = (sum << 5) + sum + gen_pattern_byte(file_id, i);
  }
  return sum;
}

// ============== Command Handlers ==============

static void cmd_help(int argc, char **argv) {
  (void)argc; (void)argv;
  printf("\nCommands:\n");
  for (unsigned i = 0; i < NUM_COMMANDS; i++) {
    printf("  %-28s %s", commands[i].usage, commands[i].desc);
    if (commands[i].alias)
      printf("  (alias: %s)", commands[i].alias);
    printf("\n");
  }
  printf("\n");
}

static void cmd_ls(int argc, char **argv) {
  (void)argc; (void)argv;
  f12_dir_t dir;
  f12_stat_t st;

  f12_err_t err = f12_opendir(&fs, "/", &dir);
  if (err != F12_OK) {
    printf("Error: %s\n", f12_strerror(err));
    return;
  }

  int count = 0;
  uint32_t total_bytes = 0;
  while (f12_readdir(&dir, &st) == F12_OK) {
    if (st.is_dir)
      printf("  %-12s    <DIR>\n", st.name);
    else
      printf("  %-12s %8lu\n", st.name, st.size);
    total_bytes += st.size;
    count++;
  }
  f12_closedir(&dir);

  if (count == 0) {
    printf("  (empty)\n");
  }

  uint16_t free_cl = count_free_clusters();
  uint32_t free_bytes = (uint32_t)free_cl * fs.fat.bpb.sectors_per_cluster * fs.fat.bpb.bytes_per_sector;
  printf("  %d file(s), %lu bytes used, %lu bytes free\n", count, total_bytes, free_bytes);
}

static void cmd_cat(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: cat <file>\n");
    return;
  }
  char name[13];
  strncpy(name, argv[1], sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  upcase(name);

  f12_file_t *file = f12_open(&fs, name, "r");
  if (!file) {
    printf("Error: %s\n", f12_strerror(f12_errno(&fs)));
    return;
  }

  int total = 0;
  int n;
  while ((n = f12_read(file, io_buf, IO_BUF_SIZE)) > 0) {
    for (int i = 0; i < n; i++) {
      putchar(io_buf[i]);
    }
    total += n;
  }
  printf("\n(%d bytes)\n", total);
  f12_close(file);
}

static void cmd_hexdump(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: hexdump <file>\n");
    return;
  }
  char name[13];
  strncpy(name, argv[1], sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  upcase(name);

  f12_file_t *file = f12_open(&fs, name, "r");
  if (!file) {
    printf("Error: %s\n", f12_strerror(f12_errno(&fs)));
    return;
  }

  uint32_t offset = 0;
  int n;
  while ((n = f12_read(file, io_buf, 16)) > 0) {
    printf("  %08lX: ", offset);
    for (int i = 0; i < 16; i++) {
      if (i < n)
        printf("%02X ", io_buf[i]);
      else
        printf("   ");
      if (i == 7) printf(" ");
    }
    printf(" |");
    for (int i = 0; i < n; i++) {
      putchar((io_buf[i] >= 32 && io_buf[i] < 127) ? io_buf[i] : '.');
    }
    printf("|\n");
    offset += n;
  }
  printf("  %lu bytes\n", offset);
  f12_close(file);
}

static void cmd_write(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: write <file>\n");
    return;
  }
  char name[13];
  strncpy(name, argv[1], sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  upcase(name);

  f12_file_t *file = f12_open(&fs, name, "w");
  if (!file) {
    printf("Error: %s\n", f12_strerror(f12_errno(&fs)));
    return;
  }

  printf("Enter text (end with . on its own line):\n");
  uint32_t total = 0;
  char line[CMD_BUF_SIZE];

  for (;;) {
    cli_readline(line, sizeof(line));
    if (strcmp(line, ".") == 0) break;

    int len = strlen(line);
    line[len] = '\n';
    len++;

    int n = f12_write(file, line, len);
    if (n < 0) {
      printf("Write error: %s\n", f12_strerror(f12_errno(&fs)));
      break;
    }
    total += n;
  }

  f12_close(file);
  printf("Wrote %lu bytes to %s\n", total, name);
}

static void cmd_rm(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: rm <file>\n");
    return;
  }
  char name[13];
  strncpy(name, argv[1], sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  upcase(name);

  f12_err_t err = f12_delete(&fs, name);
  if (err != F12_OK)
    printf("Error: %s\n", f12_strerror(err));
  else
    printf("Deleted %s\n", name);
}

static void cmd_cp(int argc, char **argv) {
  if (argc < 3) {
    printf("Usage: cp <src> <dst>\n");
    return;
  }
  char src[13], dst[13];
  strncpy(src, argv[1], sizeof(src) - 1); src[sizeof(src) - 1] = '\0';
  strncpy(dst, argv[2], sizeof(dst) - 1); dst[sizeof(dst) - 1] = '\0';
  upcase(src);
  upcase(dst);

  f12_file_t *rf = f12_open(&fs, src, "r");
  if (!rf) {
    printf("Error opening %s: %s\n", src, f12_strerror(f12_errno(&fs)));
    return;
  }

  f12_file_t *wf = f12_open(&fs, dst, "w");
  if (!wf) {
    printf("Error creating %s: %s\n", dst, f12_strerror(f12_errno(&fs)));
    f12_close(rf);
    return;
  }

  uint32_t total = 0;
  int n;
  while ((n = f12_read(rf, io_buf, IO_BUF_SIZE)) > 0) {
    int w = f12_write(wf, io_buf, n);
    if (w < 0) {
      printf("Write error: %s\n", f12_strerror(f12_errno(&fs)));
      break;
    }
    total += w;
  }

  f12_close(rf);
  f12_close(wf);
  printf("Copied %lu bytes: %s -> %s\n", total, src, dst);
}

static void cmd_mv(int argc, char **argv) {
  if (argc < 3) {
    printf("Usage: mv <src> <dst>\n");
    return;
  }
  char src[13], dst[13];
  strncpy(src, argv[1], sizeof(src) - 1); src[sizeof(src) - 1] = '\0';
  strncpy(dst, argv[2], sizeof(dst) - 1); dst[sizeof(dst) - 1] = '\0';
  upcase(src);
  upcase(dst);

  // Copy
  f12_file_t *rf = f12_open(&fs, src, "r");
  if (!rf) {
    printf("Error opening %s: %s\n", src, f12_strerror(f12_errno(&fs)));
    return;
  }

  f12_file_t *wf = f12_open(&fs, dst, "w");
  if (!wf) {
    printf("Error creating %s: %s\n", dst, f12_strerror(f12_errno(&fs)));
    f12_close(rf);
    return;
  }

  uint32_t total = 0;
  int n;
  while ((n = f12_read(rf, io_buf, IO_BUF_SIZE)) > 0) {
    int w = f12_write(wf, io_buf, n);
    if (w < 0) {
      printf("Write error: %s\n", f12_strerror(f12_errno(&fs)));
      f12_close(rf);
      f12_close(wf);
      return;
    }
    total += w;
  }

  f12_close(rf);
  f12_close(wf);

  // Delete source
  f12_err_t err = f12_delete(&fs, src);
  if (err != F12_OK) {
    printf("Warning: copied but failed to delete %s: %s\n", src, f12_strerror(err));
    return;
  }
  printf("Moved %lu bytes: %s -> %s\n", total, src, dst);
}

static void cmd_stat(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: stat <file>\n");
    return;
  }
  char name[13];
  strncpy(name, argv[1], sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  upcase(name);

  f12_stat_t st;
  f12_err_t err = f12_stat(&fs, name, &st);
  if (err != F12_OK) {
    printf("Error: %s\n", f12_strerror(err));
    return;
  }

  printf("  Name:   %s\n", st.name);
  printf("  Size:   %lu bytes\n", st.size);
  printf("  Attr:   0x%02X", st.attr);
  if (st.attr & 0x01) printf(" RO");
  if (st.attr & 0x02) printf(" HID");
  if (st.attr & 0x04) printf(" SYS");
  if (st.attr & 0x08) printf(" VOL");
  if (st.attr & 0x10) printf(" DIR");
  if (st.attr & 0x20) printf(" ARC");
  printf("\n");

  // Walk cluster chain
  fat12_dirent_t de;
  fat12_err_t ferr = fat12_find(&fs.fat, name, &de);
  if (ferr == FAT12_OK) {
    printf("  Chain:  ");
    uint16_t cluster = de.start_cluster;
    int count = 0;
    while (cluster >= 2 && !fat12_is_eof(cluster) && count < 50) {
      if (count > 0) printf(" -> ");
      printf("%u", cluster);
      uint16_t next;
      if (fat12_get_entry(&fs.fat, cluster, &next) != FAT12_OK) break;
      cluster = next;
      count++;
    }
    if (count >= 50) printf(" ...");
    printf("\n  Clusters: %d\n", count);
  }
}

static void cmd_format(int argc, char **argv) {
  const char *label = "PICODISK";
  bool full = false;

  if (argc >= 2) {
    if (strcasecmp(argv[argc - 1], "full") == 0) {
      full = true;
      if (argc >= 3) label = argv[1];
    } else {
      label = argv[1];
    }
  }

  printf("Format disk as \"%s\" (%s)? [y/N] ", label, full ? "full" : "quick");

  char line[CMD_BUF_SIZE];
  cli_readline(line, sizeof(line));
  if (line[0] != 'y' && line[0] != 'Y') {
    printf("Cancelled.\n");
    return;
  }

  if (mounted) {
    f12_unmount(&fs);
    mounted = false;
  }

  setup_io();

  // Need to upcase the label for FAT
  char ulabel[12];
  strncpy(ulabel, label, 11);
  ulabel[11] = '\0';
  upcase(ulabel);

  f12_err_t err = f12_format(&fs, ulabel, full);
  if (err != F12_OK) {
    printf("Format error: %s\n", f12_strerror(err));
    return;
  }
  printf("Format complete.\n");

  // Auto-mount
  err = do_mount();
  if (err == F12_OK) {
    mounted = true;
    printf("Mounted.\n");
  } else {
    printf("Mount error: %s\n", f12_strerror(err));
  }
}

static void cmd_mount(int argc, char **argv) {
  (void)argc; (void)argv;
  if (mounted) {
    f12_unmount(&fs);
    mounted = false;
  }

  f12_err_t err = do_mount();
  if (err != F12_OK) {
    printf("Mount error: %s\n", f12_strerror(err));
  } else {
    printf("Mounted.\n");
    mounted = true;
  }
}

static void cmd_unmount(int argc, char **argv) {
  (void)argc; (void)argv;
  if (!mounted) {
    printf("Not mounted.\n");
    return;
  }
  f12_unmount(&fs);
  mounted = false;
  printf("Unmounted.\n");
}

static void cmd_status(int argc, char **argv) {
  (void)argc; (void)argv;
  printf("  Drive:\n");
  printf("    Write protected: %s\n", floppy_write_protected(&floppy) ? "YES" : "no");
  printf("    Disk changed:    %s\n", floppy_disk_changed(&floppy) ? "YES" : "no");
  printf("    Current track:   %d\n", floppy_current_track(&floppy));
  printf("    At track 0:      %s\n", floppy_at_track0(&floppy) ? "yes" : "no");
  printf("    Motor:           %s\n", floppy.motor_on ? "ON" : "off");
  if (floppy.motor_on) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t idle_ms = now - floppy.last_io_time_ms;
    uint32_t remaining = (idle_ms < FLOPPY_IDLE_TIMEOUT_MS) ?
                         (FLOPPY_IDLE_TIMEOUT_MS - idle_ms) / 1000 : 0;
    printf("    Idle:            %lus (off in %lus)\n", idle_ms / 1000, remaining);
  }

  if (!mounted) {
    printf("  Filesystem: not mounted\n");
    return;
  }

  fat12_bpb_t *bpb = &fs.fat.bpb;
  printf("  BPB:\n");
  printf("    Bytes/sector:     %d\n", bpb->bytes_per_sector);
  printf("    Sectors/cluster:  %d\n", bpb->sectors_per_cluster);
  printf("    Reserved sectors: %d\n", bpb->reserved_sectors);
  printf("    FATs:             %d\n", bpb->num_fats);
  printf("    Root entries:     %d\n", bpb->root_entries);
  printf("    Total sectors:    %d\n", bpb->total_sectors);
  printf("    Media descriptor: 0x%02X\n", bpb->media_descriptor);
  printf("    Sectors/FAT:      %d\n", bpb->sectors_per_fat);
  printf("    Sectors/track:    %d\n", bpb->sectors_per_track);
  printf("    Heads:            %d\n", bpb->num_heads);

  uint16_t free_cl = count_free_clusters();
  uint32_t free_bytes = (uint32_t)free_cl * bpb->sectors_per_cluster * bpb->bytes_per_sector;
  printf("  Free: %lu bytes (%d clusters)\n", free_bytes, free_cl);
}

static void touch_io_time(void) {
  floppy.last_io_time_ms = to_ms_since_boot(get_absolute_time());
}

static void cmd_motor(int argc, char **argv) {
  if (argc < 2) {
    printf("Motor is %s\n", floppy.motor_on ? "ON" : "off");
    return;
  }
  if (strcasecmp(argv[1], "on") == 0) {
    touch_io_time();
    floppy_motor_on(&floppy);
    printf("Motor ON\n");
  } else if (strcasecmp(argv[1], "off") == 0) {
    floppy_motor_off(&floppy);
    printf("Motor off\n");
  } else {
    printf("Usage: motor [on|off]\n");
  }
}

static void cmd_select(int argc, char **argv) {
  if (argc < 2) {
    printf("Drive is %s\n", floppy.selected ? "selected" : "deselected");
    return;
  }
  if (strcasecmp(argv[1], "on") == 0) {
    touch_io_time();
    floppy_select(&floppy, true);
    printf("Drive selected\n");
  } else if (strcasecmp(argv[1], "off") == 0) {
    floppy_select(&floppy, false);
    printf("Drive deselected\n");
  } else {
    printf("Usage: select [on|off]\n");
  }
}

static void cmd_home(int argc, char **argv) {
  (void)argc; (void)argv;
  printf("Seeking to track 0...\n");
  floppy_status_t st = floppy_seek(&floppy, 0);
  if (st == FLOPPY_OK)
    printf("At track 0 (TRK0 pin: %s)\n",
           floppy_at_track0(&floppy) ? "active" : "NOT active");
  else
    printf("Seek error: %d\n", st);
}

static void cmd_pins(int argc, char **argv) {
  (void)argc; (void)argv;
  printf("  GPIO  Pin  Signal          State\n");
  printf("  ----  ---  ------          -----\n");

  struct { uint gpio; const char *fpin; const char *name; bool is_input; } pins[] = {
    {floppy.pins.index,         " 8", "INDEX",         true},
    {floppy.pins.track0,        "26", "TRACK0",        true},
    {floppy.pins.write_protect, "28", "WRITE_PROTECT", true},
    {floppy.pins.read_data,     "30", "READ_DATA",     true},
    {floppy.pins.disk_change,   "34", "DISK_CHANGE",   true},
    {floppy.pins.drive_select,  "12", "DRIVE_SELECT",  false},
    {floppy.pins.motor_enable,  "10", "MOTOR_ENABLE",  false},
    {floppy.pins.direction,     "18", "DIRECTION",      false},
    {floppy.pins.step,          "20", "STEP",           false},
    {floppy.pins.write_data,    "22", "WRITE_DATA",     false},
    {floppy.pins.write_gate,    "24", "WRITE_GATE",     false},
    {floppy.pins.side_select,   "32", "SIDE_SELECT",    false},
    {floppy.pins.density,       " 2", "DENSITY",        false},
  };

  for (unsigned i = 0; i < sizeof(pins)/sizeof(pins[0]); i++) {
    bool val = gpio_get(pins[i].gpio);
    printf("  GP%-2d  %s   %-15s %d (%s)%s\n",
           pins[i].gpio, pins[i].fpin, pins[i].name,
           val, val ? "HIGH" : "LOW",
           pins[i].is_input ? " <input>" : "");
  }
}

static void cmd_poll(int argc, char **argv) {
  (void)argc; (void)argv;

  touch_io_time();
  if (!floppy.motor_on || !floppy.selected) {
    printf("  Starting motor and selecting drive...\n");
    floppy_select(&floppy, true);
    floppy_motor_on(&floppy);
  }

  uint pin = floppy.pins.read_data;
  uint ix_pin = floppy.pins.index;
  printf("  Polling GP%d (read_data) and GP%d (index) for 2 seconds...\n", pin, ix_pin);

  int transitions = 0;
  int ix_transitions = 0;
  bool prev = gpio_get(pin);
  bool ix_prev = gpio_get(ix_pin);
  absolute_time_t deadline = make_timeout_time_ms(2000);

  while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
    bool now = gpio_get(pin);
    bool ix_now = gpio_get(ix_pin);
    if (now != prev) {
      transitions++;
      prev = now;
    }
    if (ix_now != ix_prev) {
      ix_transitions++;
      ix_prev = ix_now;
    }
  }

  printf("  read_data transitions: %d  (expect ~200k+ if disk present)\n", transitions);
  printf("  index transitions:     %d  (expect ~24 for 360rpm)\n", ix_transitions);
  if (transitions == 0)
    printf("  No activity on read_data -- check wiring or disk.\n");
}

static void cmd_flux(int argc, char **argv) {
  int count = 200;
  if (argc >= 2) count = atoi(argv[1]);
  if (count < 1) count = 1;
  if (count > 10000) count = 10000;

  touch_io_time();
  if (!floppy.motor_on || !floppy.selected) {
    printf("  Starting motor and selecting drive...\n");
    floppy_select(&floppy, true);
    floppy_motor_on(&floppy);
  }

  printf("  Reading %d raw flux transitions (5s timeout)...\n", count);
  printf("  read_data=GP%d  index=GP%d\n", floppy.pins.read_data, floppy.pins.index);

  pio_sm_exec(floppy.read.pio, floppy.read.sm, pio_encode_jmp(floppy.read.offset));
  pio_sm_restart(floppy.read.pio, floppy.read.sm);
  pio_sm_clear_fifos(floppy.read.pio, floppy.read.sm);
  floppy.read.half = 0;
  pio_sm_set_enabled(floppy.read.pio, floppy.read.sm, true);

  // Wait for first transition with timeout
  absolute_time_t deadline = make_timeout_time_ms(5000);
  while (!flux_data_available()) {
    if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
      printf("  TIMEOUT: no flux data received.\n");
      printf("  Check: disk inserted? read_data wiring? motor spinning?\n");
      printf("  Current read_data (GP%d) = %d\n",
             floppy.pins.read_data, gpio_get(floppy.pins.read_data));
      pio_sm_set_enabled(floppy.read.pio, floppy.read.sm, false);
      return;
    }
    tight_loop_contents();
  }

  uint16_t prev = flux_read_raw() >> 1;

  for (int i = 0; i < count; i++) {
    deadline = make_timeout_time_ms(1000);
    while (!flux_data_available()) {
      if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
        printf("  TIMEOUT after %d transitions.\n", i);
        pio_sm_set_enabled(floppy.read.pio, floppy.read.sm, false);
        return;
      }
      tight_loop_contents();
    }

    uint16_t value = flux_read_raw();
    uint8_t ix = value & 1;
    uint16_t cnt = value >> 1;
    int delta = prev - cnt;
    if (delta < 0) delta += 0x8000;
    prev = cnt;

    printf("  %4d: delta=%3d  ix=%d  raw=0x%04X\n", i, delta, ix, value);
  }

  pio_sm_set_enabled(floppy.read.pio, floppy.read.sm, false);
  printf("  Done.\n");
}

static void cmd_seek(int argc, char **argv) {
  if (argc < 2) {
    printf("Usage: seek <track>\n");
    return;
  }
  int track = atoi(argv[1]);
  if (track < 0 || track > 79) {
    printf("Track must be 0-79\n");
    return;
  }
  floppy_status_t st = floppy_seek(&floppy, track);
  if (st != FLOPPY_OK)
    printf("Seek error: %d\n", st);
  else
    printf("Head at track %d\n", track);
}

static void cmd_dump(int argc, char **argv) {
  if (argc < 3) {
    printf("Usage: dump <track> <side> [sector]\n");
    return;
  }
  int track = atoi(argv[1]);
  int side = atoi(argv[2]);
  int sec_start = 1, sec_end = SECTORS_PER_TRACK;

  if (argc >= 4) {
    sec_start = atoi(argv[3]);
    sec_end = sec_start;
  }

  if (track < 0 || track > 79 || side < 0 || side > 1 ||
      sec_start < 1 || sec_end > SECTORS_PER_TRACK) {
    printf("Invalid: track 0-79, side 0-1, sector 1-%d\n", SECTORS_PER_TRACK);
    return;
  }

  sector_t sector;
  for (int s = sec_start; s <= sec_end; s++) {
    sector.track = track;
    sector.side = side;
    sector.sector_n = s;
    sector.valid = false;

    floppy_status_t st = floppy_read_sector(&floppy, &sector);
    printf("  --- T%d/S%d/Sec%d %s ---\n", track, side, s,
           (st == FLOPPY_OK && sector.valid) ? "OK" : "FAIL");

    if (st != FLOPPY_OK || !sector.valid) continue;

    for (int row = 0; row < 32; row++) {
      int off = row * 16;
      printf("  %03X: ", off);
      for (int i = 0; i < 16; i++) {
        printf("%02X ", sector.data[off + i]);
        if (i == 7) printf(" ");
      }
      printf(" |");
      for (int i = 0; i < 16; i++) {
        uint8_t c = sector.data[off + i];
        putchar((c >= 32 && c < 127) ? c : '.');
      }
      printf("|\n");
    }
  }
}

static void cmd_mfm(int argc, char **argv) {
  if (argc < 3) {
    printf("Usage: mfm <track> <side>\n");
    return;
  }
  int track = atoi(argv[1]);
  int side = atoi(argv[2]);

  if (track < 0 || track > 79 || side < 0 || side > 1) {
    printf("Invalid: track 0-79, side 0-1\n");
    return;
  }

  printf("  Analyzing track %d side %d...\n", track, side);
  track_stats_t stats;
  read_track_stats(track, side, &stats);

  printf("  Pulses:   %lu total\n", stats.total_pulses);
  printf("  Short:    %lu (%.1f%%)\n", stats.short_count,
         stats.total_pulses ? stats.short_count * 100.0 / stats.total_pulses : 0);
  printf("  Medium:   %lu (%.1f%%)\n", stats.medium_count,
         stats.total_pulses ? stats.medium_count * 100.0 / stats.total_pulses : 0);
  printf("  Long:     %lu (%.1f%%)\n", stats.long_count,
         stats.total_pulses ? stats.long_count * 100.0 / stats.total_pulses : 0);
  printf("  Invalid:  %lu (%.1f%%)\n", stats.invalid_count,
         stats.total_pulses ? stats.invalid_count * 100.0 / stats.total_pulses : 0);
  printf("  Syncs:    %lu\n", stats.syncs);
  printf("  Sectors:  %lu / %d\n", stats.sectors, SECTORS_PER_TRACK);
  printf("  CRC err:  %lu\n", stats.crc_errors);
  printf("  Adaptive: T2_max=%d  T3_max=%d\n", stats.T2_max, stats.T3_max);

  print_histogram(&stats);
}

static void cmd_selftest(int argc, char **argv) {
  (void)argc; (void)argv;

  printf("This will FORMAT the disk and run write/read/verify.\n");
  printf("Continue? [y/N] ");

  char line[CMD_BUF_SIZE];
  cli_readline(line, sizeof(line));
  if (line[0] != 'y' && line[0] != 'Y') {
    printf("Cancelled.\n");
    return;
  }

  // Unmount if mounted
  if (mounted) {
    f12_unmount(&fs);
    mounted = false;
  }

  int pass = 0, fail = 0;

  // Format
  printf("  Formatting...\n");
  setup_io();
  f12_err_t err = f12_format(&fs, "SELFTEST", false);
  if (err != F12_OK) {
    printf("  FAIL: format: %s\n", f12_strerror(err));
    return;
  }
  printf("  PASS: format\n");
  pass++;

  // Mount
  err = do_mount();
  if (err != F12_OK) {
    printf("  FAIL: mount: %s\n", f12_strerror(err));
    return;
  }
  printf("  PASS: mount\n");
  pass++;
  mounted = true;

  // Test files
  struct { const char *name; uint32_t size; } tests[] = {
    {"TINY.BIN",  1},
    {"SMALL.DAT", 100},
    {"SECT.DAT",  512},
    {"MULTI.DAT", 2048},
    {"BIG.DAT",   SELF_BUF_SIZE},
  };
  int ntests = sizeof(tests) / sizeof(tests[0]);

  // Write
  printf("  Writing %d test files...\n", ntests);
  for (int i = 0; i < ntests; i++) {
    // Fill pattern into self_buf (up to SELF_BUF_SIZE)
    uint32_t sz = tests[i].size;
    for (uint32_t j = 0; j < sz; j++) {
      self_buf[j] = gen_pattern_byte(i, j);
    }

    f12_file_t *f = f12_open(&fs, tests[i].name, "w");
    if (!f) {
      printf("  FAIL: open %s for write: %s\n", tests[i].name, f12_strerror(f12_errno(&fs)));
      fail++;
      continue;
    }
    f12_write_full(f, self_buf, sz);
    f12_close(f);
    printf("    wrote %s (%lu bytes)\n", tests[i].name, sz);
  }

  // Read back + verify
  printf("  Verifying...\n");
  for (int i = 0; i < ntests; i++) {
    uint32_t sz = tests[i].size;
    uint32_t expected_cksum = pattern_checksum(i, sz);

    f12_file_t *f = f12_open(&fs, tests[i].name, "r");
    if (!f) {
      printf("  FAIL: open %s for read\n", tests[i].name);
      fail++;
      continue;
    }

    uint32_t got = f12_read_full(f, self_buf, sz);
    f12_close(f);

    uint32_t actual_cksum = checksum_buf(self_buf, got);
    bool ok = (got == sz && actual_cksum == expected_cksum);
    printf("  %s: %s size=%lu cksum=0x%08lX\n",
           ok ? "PASS" : "FAIL", tests[i].name, got, actual_cksum);
    if (ok) pass++; else fail++;
  }

  // Clean up
  printf("  Cleaning up...\n");
  for (int i = 0; i < ntests; i++) {
    err = f12_delete(&fs, tests[i].name);
    if (err != F12_OK) {
      printf("  FAIL: delete %s: %s\n", tests[i].name, f12_strerror(err));
      fail++;
    } else {
      pass++;
    }
  }

  printf("\n  Results: %d passed, %d failed -- %s\n",
         pass, fail, fail == 0 ? "ALL PASSED" : "SOME FAILED");
}

static void cmd_reboot(int argc, char **argv) {
  (void)argc; (void)argv;
  printf("Rebooting...\n");
  sleep_ms(100);
  watchdog_reboot(0, 0, 0);
  for (;;) tight_loop_contents();
}

// ============== Main ==============

int main(void) {
  stdio_init_all();
  sleep_ms(2000);

  printf("\r\n\r\n=== Pico Floppy Shell ===\r\n");

  floppy = (floppy_t){
    .pins = {
      .index         = 14,
      .track0        = 5,
      .write_protect = 4,
      .read_data     = 3,
      .disk_change   = 1,
      .drive_select  = 12,  // floppy pin 12 (DRVSB)
      .motor_enable  = 10,  // floppy pin 16 (MOTEB)
      .direction     = 9,
      .step          = 8,
      .write_data    = 7,
      .write_gate    = 6,
      .side_select   = 2,
      .density       = 15,
    }
  };

  floppy_init(&floppy);
  floppy_set_density(&floppy, true);

  printf("Drive initialized (HD mode)\r\n");
  printf("Type 'help' for commands, 'mount' when disk is ready.\r\n\r\n");

  mounted = false;

  char *argv[MAX_ARGS];

  for (;;) {
    print_prompt();
    int len = cli_readline(cmd_buf, CMD_BUF_SIZE);
    if (len == 0) continue;

    int argc = tokenize(cmd_buf, argv, MAX_ARGS);
    if (argc == 0) continue;

    const cmd_entry_t *cmd = find_command(argv[0]);
    if (!cmd) {
      printf("Unknown command '%s'. Type 'help' for commands.\n", argv[0]);
      continue;
    }

    if (cmd->needs_mount && !mounted) {
      printf("Not mounted. Use 'mount' first.\n");
      continue;
    }

    cmd->fn(argc, argv);
  }

  return 0;
}
