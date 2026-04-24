#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/clocks.h"
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
#define SELF_BUF_SIZE 50000
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
static void cmd_selftest2(int argc, char **argv);
static void cmd_selftest3(int argc, char **argv);
static void cmd_selftest3_a(int argc, char **argv);
static void cmd_selftest3_b(int argc, char **argv);
static void cmd_test_full(int argc, char **argv);
static void cmd_starwars(int argc, char **argv);
static void cmd_diskdump(int argc, char **argv);
static void cmd_mfmscan(int argc, char **argv);
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
  {"selftest2",NULL,   cmd_selftest2,false,"selftest2 <n> <size>","Stress: n rounds of write/delete/verify"},
  {"selftest3",NULL,   cmd_selftest3,false,"selftest3 [rounds]",  "Patch-specific full-disk overwrite test"},
  {"selftest3-a",NULL, cmd_selftest3_a,false,"selftest3-a [rounds]","Format + prepare reboot-resume overwrite test"},
  {"selftest3-b",NULL, cmd_selftest3_b,false,"selftest3-b [rounds]","Resume selftest3 after power cycle without format"},
  {"test-full",NULL,   cmd_test_full,false,"test-full [rounds]", "Full hardware test sequence, no selftest3"},
  {"starwars", NULL,   cmd_starwars, false,"starwars",            "Imperial March on the stepper motor"},
  {"diskdump",NULL,    cmd_diskdump,false, "diskdump [quiet]",    "Full disk sector scan + retry summary"},
  {"mfmscan", NULL,    cmd_mfmscan, false, "mfmscan",             "MFM signal quality across all tracks"},
  {"reboot",  NULL,    cmd_reboot,  false, "reboot",              "Reboot the Pico"},
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

// ============== IO Helpers ==============

static f12_err_t do_mount(void) {
  f12_io_t io = {
    .read = floppy_io_read,
    .read_track = floppy_io_read_track,
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
    .read_track = floppy_io_read_track,
    .write = floppy_io_write,
    .disk_changed = floppy_io_disk_changed,
    .write_protected = floppy_io_write_protected,
    .ctx = &floppy,
  };
}

static uint32_t f12_write_full(f12_file_t *f, const void *buf, uint32_t len) {
  uint32_t written = 0;
  while (written < len) {
    uint32_t chunk = len - written;
    if (chunk > 512) chunk = 512;
    int n = f12_write(f, (const uint8_t *)buf + written, chunk);
    if (n <= 0) break;
    written += n;
  }
  return written;
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
  uint32_t sector_records;
  uint32_t unique_sectors;
  uint32_t crc_errors;
  bool seen[SECTORS_PER_TRACK];
} track_stats_t;

typedef struct {
  int total_valid;
  int total_invalid;
  uint32_t checksum;
  floppy_stats_t retries;
} diskdump_stats_t;

static diskdump_stats_t run_diskdump(bool verbose);

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

    if (mfm_feed(&mfm, delta, &sector) && sector.valid &&
        sector.sector_n >= 1 && sector.sector_n <= SECTORS_PER_TRACK) {
      uint8_t idx = sector.sector_n - 1;
      if (!stats->seen[idx]) {
        stats->seen[idx] = true;
        stats->unique_sectors++;
      }
    }
    prev = cnt;
  }

  pio_sm_set_enabled(floppy.read.pio, floppy.read.sm, false);

  stats->T2_max = mfm.T2_max;
  stats->T3_max = mfm.T3_max;
  stats->syncs = mfm.syncs_found;
  stats->sector_records = mfm.sectors_read;
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

static void fill_pattern_range(uint8_t *buf, int file_id, uint32_t offset, uint32_t size) {
  for (uint32_t i = 0; i < size; i++) {
    buf[i] = gen_pattern_byte(file_id, offset + i);
  }
}

static uint32_t fs_cluster_size(void) {
  return (uint32_t)fs.fat.bpb.sectors_per_cluster * fs.fat.bpb.bytes_per_sector;
}

static uint32_t clusters_for_size(uint32_t size) {
  uint32_t cluster_size = fs_cluster_size();
  if (size == 0) return 0;
  return (size + cluster_size - 1) / cluster_size;
}

static bool write_pattern_file_chunked(const char *name, int file_id, uint32_t size,
                                       uint32_t chunk_size, uint32_t *written_out,
                                       f12_err_t *close_err_out) {
  if (chunk_size == 0 || chunk_size > SELF_BUF_SIZE) {
    chunk_size = SELF_BUF_SIZE;
  }

  f12_file_t *f = f12_open(&fs, name, "w");
  if (!f) {
    if (written_out) *written_out = 0;
    if (close_err_out) *close_err_out = f12_errno(&fs);
    return false;
  }

  uint32_t written = 0;
  while (written < size) {
    uint32_t chunk = size - written;
    if (chunk > chunk_size) chunk = chunk_size;
    fill_pattern_range(self_buf, file_id, written, chunk);

    int n = f12_write(f, self_buf, chunk);
    if (n <= 0) break;
    written += (uint32_t)n;
  }

  if (written_out) *written_out = written;
  if (close_err_out) *close_err_out = f12_close(f);
  else f12_close(f);
  return true;
}

static bool verify_pattern_file_chunked(const char *name, int file_id, uint32_t size,
                                        uint32_t chunk_size, uint32_t *read_out) {
  if (chunk_size == 0 || chunk_size > SELF_BUF_SIZE) {
    chunk_size = SELF_BUF_SIZE;
  }

  f12_file_t *f = f12_open(&fs, name, "r");
  if (!f) {
    if (read_out) *read_out = 0;
    return false;
  }

  bool ok = true;
  uint32_t total = 0;
  while (total < size) {
    uint32_t chunk = size - total;
    if (chunk > chunk_size) chunk = chunk_size;

    int n = f12_read(f, self_buf, chunk);
    if (n <= 0) {
      ok = false;
      break;
    }

    for (int i = 0; i < n; i++) {
      if (self_buf[i] != gen_pattern_byte(file_id, total + (uint32_t)i)) {
        ok = false;
        break;
      }
    }
    if (!ok) break;

    total += (uint32_t)n;
  }

  if (read_out) *read_out = total;
  if (f12_close(f) != F12_OK) ok = false;
  return ok && total == size;
}

static bool verify_pattern_at(f12_file_t *f, int file_id, uint32_t offset, uint32_t len) {
  if (len > SELF_BUF_SIZE) return false;

  int n = f12_read_at(f, offset, self_buf, len);
  if (n != (int)len) return false;

  for (uint32_t i = 0; i < len; i++) {
    if (self_buf[i] != gen_pattern_byte(file_id, offset + i)) {
      return false;
    }
  }

  return true;
}

static bool verify_pattern_windows(const char *name, int file_id, uint32_t file_size) {
  if (file_size == 0) return false;

  uint32_t window = fs_cluster_size();
  if (window == 0 || window > SELF_BUF_SIZE) window = SELF_BUF_SIZE;
  if (window > file_size) window = file_size;

  uint32_t offsets[3] = {
    0,
    file_size / 2,
    file_size - window,
  };

  f12_file_t *f = f12_open(&fs, name, "r");
  if (!f) return false;

  bool ok = true;
  for (int i = 0; i < 3; i++) {
    uint32_t offset = offsets[i];
    if (offset + window > file_size) {
      offset = file_size - window;
    }
    if (!verify_pattern_at(f, file_id, offset, window)) {
      ok = false;
      break;
    }
  }

  if (f12_close(f) != F12_OK) ok = false;
  return ok;
}

#define SELFTEST3_STATE_FILE "SELF3.STA"
#define SELFTEST3_STATE_MAGIC "S3STATE1"
#define SELFTEST3_STATE_VERSION 1u
#define SELFTEST3_STAGE_A_READY 1u
#define SELFTEST3_STAGE_B_DONE  2u
#define SELFTEST3_RANDOM_SEED   0x13579BDFu
#define SELFTEST3_BYTE_FILE     "BYTEIO.BIN"
#define SELFTEST3_TARGET_FILE   "TARGET.BIN"
#define SELFTEST3_FILLER_FILE   "FILLER.BIN"

typedef struct {
  char magic[8];
  uint32_t version;
  uint32_t stage;
  uint32_t seed;
  uint32_t byte_id;
  uint32_t byte_size;
  uint32_t target_id;
  uint32_t target_size;
  uint32_t filler_id;
  uint32_t filler_size;
  uint32_t cluster_size;
  uint32_t rounds_a;
  uint8_t reserved[460];
} selftest3_state_t;

_Static_assert(sizeof(selftest3_state_t) == 512, "selftest3_state_t must be 512 bytes");

static bool write_blob_file(const char *name, const void *buf, uint32_t size,
                            uint32_t chunk_size, uint32_t *written_out,
                            f12_err_t *close_err_out) {
  if (chunk_size == 0 || chunk_size > SELF_BUF_SIZE) {
    chunk_size = SELF_BUF_SIZE;
  }

  f12_file_t *f = f12_open(&fs, name, "w");
  if (!f) {
    if (written_out) *written_out = 0;
    if (close_err_out) *close_err_out = f12_errno(&fs);
    return false;
  }

  uint32_t written = 0;
  while (written < size) {
    uint32_t chunk = size - written;
    if (chunk > chunk_size) chunk = chunk_size;

    int n = f12_write(f, (const uint8_t *)buf + written, chunk);
    if (n <= 0) break;
    written += (uint32_t)n;
  }

  if (written_out) *written_out = written;
  if (close_err_out) *close_err_out = f12_close(f);
  else f12_close(f);
  return true;
}

static bool read_blob_file(const char *name, void *buf, uint32_t size, uint32_t *read_out) {
  f12_file_t *f = f12_open(&fs, name, "r");
  if (!f) {
    if (read_out) *read_out = 0;
    return false;
  }

  uint32_t total = f12_read_full(f, buf, size);
  bool ok = (f12_close(f) == F12_OK);

  if (read_out) *read_out = total;
  return ok && total == size;
}

static bool selftest3_store_state(const selftest3_state_t *state) {
  uint32_t written = 0;
  f12_err_t close_err = F12_OK;
  return write_blob_file(SELFTEST3_STATE_FILE, state, sizeof(*state), 37,
                         &written, &close_err) &&
         written == sizeof(*state) &&
         close_err == F12_OK;
}

static bool selftest3_load_state(selftest3_state_t *state) {
  uint32_t got = 0;
  if (!read_blob_file(SELFTEST3_STATE_FILE, state, sizeof(*state), &got)) {
    return false;
  }

  return got == sizeof(*state) &&
         memcmp(state->magic, SELFTEST3_STATE_MAGIC, sizeof(state->magic)) == 0 &&
         state->version == SELFTEST3_STATE_VERSION;
}

static uint32_t selftest3_rand(uint32_t *state) {
  uint32_t x = *state ? *state : 1u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static uint32_t selftest3_random_chunk(uint32_t *rng) {
  static const uint16_t choices[] = {1, 2, 3, 5, 7, 19, 37, 73, 113, 257, 509};
  return choices[selftest3_rand(rng) % (sizeof(choices) / sizeof(choices[0]))];
}

static uint32_t selftest3_random_pause_ms(uint32_t *rng) {
  return 200 + (selftest3_rand(rng) % 500);
}

static f12_err_t selftest3_remount_after_pause(uint32_t pause_ms) {
  if (mounted) {
    f12_unmount(&fs);
    mounted = false;
  }

  floppy_motor_off(&floppy);
  floppy_select(&floppy, false);
  if (pause_ms > 0) {
    sleep_ms(pause_ms);
  }

  f12_err_t err = do_mount();
  if (err == F12_OK) {
    mounted = true;
  }
  return err;
}

static uint32_t selftest3_random_target_size(uint32_t *rng, uint32_t cluster_size,
                                             uint32_t max_clusters, uint32_t current_clusters) {
  uint32_t clusters = 1 + (selftest3_rand(rng) % max_clusters);
  if (clusters == current_clusters) {
    clusters = (clusters % max_clusters) + 1;
  }

  if (clusters == 1) {
    return 1 + (selftest3_rand(rng) % cluster_size);
  }

  return (clusters - 1) * cluster_size + 1 + (selftest3_rand(rng) % cluster_size);
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

  printf("Enter text (end with . on its own line):\n");

  // Buffer all input first, then write once
  uint32_t pos = 0;
  char line[CMD_BUF_SIZE];

  for (;;) {
    cli_readline(line, sizeof(line));
    if (strcmp(line, ".") == 0) break;

    int len = strlen(line);
    if (pos + len + 1 > SELF_BUF_SIZE) {
      printf("Input too large (max %d bytes)\n", SELF_BUF_SIZE);
      return;
    }
    memcpy(self_buf + pos, line, len);
    pos += len;
    self_buf[pos++] = '\n';
  }

  if (pos == 0) {
    printf("Nothing to write.\n");
    return;
  }

  f12_file_t *file = f12_open(&fs, name, "w");
  if (!file) {
    printf("Error: %s\n", f12_strerror(f12_errno(&fs)));
    return;
  }

  uint32_t wrote = f12_write_full(file, self_buf, pos);
  f12_err_t cerr = f12_close(file);
  if (wrote != pos || cerr != F12_OK)
    printf("Error writing %s: wrote %lu/%lu close=%s\n", name, wrote, pos, f12_strerror(cerr));
  else
    printf("Wrote %lu bytes to %s\n", pos, name);
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
  printf("  Records:  %lu\n", stats.sector_records);
  printf("  Unique:   %lu / %d\n", stats.unique_sectors, SECTORS_PER_TRACK);
  printf("  CRC err:  %lu\n", stats.crc_errors);
  printf("  Adaptive: T2_max=%d  T3_max=%d\n", stats.T2_max, stats.T3_max);

  print_histogram(&stats);
}

static void fill_pattern(uint8_t *buf, int file_id, uint32_t size) {
  for (uint32_t i = 0; i < size; i++)
    buf[i] = gen_pattern_byte(file_id, i);
}

static void check(bool cond, const char *tag, int *pass, int *fail) {
  if (cond) {
    printf("  PASS: %s\n", tag);
    (*pass)++;
  } else {
    printf("  FAIL: %s\n", tag);
    (*fail)++;
  }
}

static bool read_file_exact(const char *name, uint8_t *buf, uint32_t expected) {
  f12_file_t *f = f12_open(&fs, name, "r");
  if (!f) return false;
  uint32_t got = f12_read_full(f, buf, expected + 1);
  bool close_ok = f12_close(f) == F12_OK;
  return close_ok && got == expected;
}

static bool mfm_health_check(int track, int side, const char *label, int *pass, int *fail) {
  track_stats_t stats;
  read_track_stats(track, side, &stats);

  printf("  %s T%d/S%d: unique=%lu/%d records=%lu crc=%lu invalid=%lu/%lu\n",
         label, track, side, stats.unique_sectors, SECTORS_PER_TRACK,
         stats.sector_records, stats.crc_errors,
         stats.invalid_count, stats.total_pulses);

  uint32_t invalid_limit = stats.total_pulses / 200;
  if (invalid_limit < 1) invalid_limit = 1;

  char tag[96];
  snprintf(tag, sizeof(tag), "%s unique sectors", label);
  check(stats.unique_sectors == SECTORS_PER_TRACK, tag, pass, fail);
  snprintf(tag, sizeof(tag), "%s CRC clean", label);
  check(stats.crc_errors == 0, tag, pass, fail);
  snprintf(tag, sizeof(tag), "%s invalid pulse threshold", label);
  check(stats.invalid_count <= invalid_limit, tag, pass, fail);

  return stats.unique_sectors == SECTORS_PER_TRACK &&
         stats.crc_errors == 0 &&
         stats.invalid_count <= invalid_limit;
}

static void cmd_test_full(int argc, char **argv) {
  int rounds = 6;
  if (argc > 2) {
    printf("Usage: test-full [rounds]\n");
    return;
  }
  if (argc == 2) {
    rounds = atoi(argv[1]);
    if (rounds < 1 || rounds > 100) {
      printf("Rounds must be 1-100\n");
      return;
    }
  }

  printf("This will FORMAT the disk, run diagnostics, file I/O, and a full sector scan.\n");
  printf("It does not run selftest3. Continue? [y/N] ");

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

  int pass = 0;
  int fail = 0;

  printf("\n--- Phase 1: GPIO and Flux Sanity ---\n");
  cmd_pins(0, NULL);
  cmd_status(0, NULL);
  check(!floppy_write_protected(&floppy), "disk is writable", &pass, &fail);
  cmd_poll(0, NULL);
  char *flux_args[] = {"flux", "50"};
  cmd_flux(2, flux_args);

  if (floppy_write_protected(&floppy)) goto done;

  printf("\n--- Phase 2: Full Format and Mount ---\n");
  setup_io();
  f12_err_t err = f12_format(&fs, "TESTFULL", true);
  check(err == F12_OK, "full format", &pass, &fail);
  if (err != F12_OK) goto done;

  err = do_mount();
  check(err == F12_OK, "mount after full format", &pass, &fail);
  if (err != F12_OK) goto done;
  mounted = true;
  cmd_status(0, NULL);

  printf("\n--- Phase 3: MFM Signal Checks ---\n");
  mfm_health_check(0, 0, "outer", &pass, &fail);
  mfm_health_check(79, 0, "inner", &pass, &fail);

  printf("\n--- Phase 4: Shell File Operations ---\n");
  const char text[] = "test-full hardware sequence\nformat mount write copy move delete verify\n";
  uint32_t text_len = sizeof(text) - 1;
  f12_file_t *f = f12_open(&fs, "TEST.TXT", "w");
  check(f != NULL, "open TEST.TXT for write", &pass, &fail);
  if (f) {
    uint32_t wrote = f12_write_full(f, text, text_len);
    f12_err_t close_err = f12_close(f);
    check(wrote == text_len && close_err == F12_OK, "write TEST.TXT", &pass, &fail);
  }

  bool exact = read_file_exact("TEST.TXT", self_buf, text_len);
  check(exact && memcmp(self_buf, text, text_len) == 0, "read TEST.TXT", &pass, &fail);

  char *cp_args[] = {"cp", "TEST.TXT", "COPY.TXT"};
  cmd_cp(3, cp_args);
  exact = read_file_exact("COPY.TXT", self_buf, text_len);
  check(exact && memcmp(self_buf, text, text_len) == 0, "copy TEST.TXT to COPY.TXT", &pass, &fail);

  char *mv_args[] = {"mv", "COPY.TXT", "MOVED.TXT"};
  cmd_mv(3, mv_args);
  f12_stat_t st;
  check(f12_stat(&fs, "COPY.TXT", &st) == F12_ERR_NOT_FOUND, "COPY.TXT removed by mv", &pass, &fail);
  exact = read_file_exact("MOVED.TXT", self_buf, text_len);
  check(exact && memcmp(self_buf, text, text_len) == 0, "MOVED.TXT verified", &pass, &fail);

  char *rm_args[] = {"rm", "MOVED.TXT"};
  cmd_rm(2, rm_args);
  check(f12_stat(&fs, "MOVED.TXT", &st) == F12_ERR_NOT_FOUND, "rm MOVED.TXT", &pass, &fail);

  printf("\n--- Phase 5: Pattern Stress ---\n");
  struct { const char *name; uint32_t size; int id; uint32_t chunk; } files[] = {
    {"BYTEIO.BIN", 4096, 1000, 1},
    {"SMALL.BIN", 512, 1001, 17},
    {"MEDIUM.BIN", 8192, 1002, 257},
    {"LARGE.BIN", 32768, 1003, 4096},
  };

  for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    uint32_t written = 0;
    f12_err_t close_err = F12_OK;
    bool ok = write_pattern_file_chunked(files[i].name, files[i].id, files[i].size,
                                         files[i].chunk, &written, &close_err);
    char tag[80];
    snprintf(tag, sizeof(tag), "write %s", files[i].name);
    check(ok && written == files[i].size && close_err == F12_OK, tag, &pass, &fail);
  }

  for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    char tag[80];
    snprintf(tag, sizeof(tag), "verify %s", files[i].name);
    check(verify_pattern_file_chunked(files[i].name, files[i].id, files[i].size,
                                      files[i].chunk, NULL),
          tag, &pass, &fail);
  }

  for (int round = 0; round < rounds; round++) {
    uint32_t size = 2048 + (uint32_t)(round % 9) * 1536;
    uint32_t chunk = 1 + (uint32_t)(round % 7) * 73;
    int id = 2000 + round;
    uint32_t written = 0;
    f12_err_t close_err = F12_OK;
    bool ok = write_pattern_file_chunked("ROUND.BIN", id, size, chunk, &written, &close_err);
    char tag[96];
    snprintf(tag, sizeof(tag), "round %d overwrite write", round + 1);
    check(ok && written == size && close_err == F12_OK, tag, &pass, &fail);

    snprintf(tag, sizeof(tag), "round %d overwrite verify", round + 1);
    check(verify_pattern_file_chunked("ROUND.BIN", id, size, chunk + 31, NULL), tag, &pass, &fail);
  }

  err = f12_delete(&fs, "SMALL.BIN");
  check(err == F12_OK, "delete SMALL.BIN", &pass, &fail);
  err = f12_delete(&fs, "MEDIUM.BIN");
  check(err == F12_OK, "delete MEDIUM.BIN", &pass, &fail);
  check(f12_stat(&fs, "SMALL.BIN", &st) == F12_ERR_NOT_FOUND, "SMALL.BIN gone", &pass, &fail);
  check(f12_stat(&fs, "MEDIUM.BIN", &st) == F12_ERR_NOT_FOUND, "MEDIUM.BIN gone", &pass, &fail);

  uint32_t written = 0;
  f12_err_t close_err = F12_OK;
  bool ok = write_pattern_file_chunked("REFILL.BIN", 3000, 12000, 333, &written, &close_err);
  check(ok && written == 12000 && close_err == F12_OK, "write REFILL.BIN", &pass, &fail);
  check(verify_pattern_file_chunked("REFILL.BIN", 3000, 12000, 777, NULL), "verify REFILL.BIN", &pass, &fail);

  printf("\n--- Phase 6: Remount Verify ---\n");
  f12_unmount(&fs);
  mounted = false;
  err = do_mount();
  check(err == F12_OK, "remount after stress", &pass, &fail);
  if (err != F12_OK) goto done;
  mounted = true;
  check(verify_pattern_file_chunked("BYTEIO.BIN", 1000, 4096, 1, NULL), "BYTEIO.BIN survives remount", &pass, &fail);
  check(verify_pattern_file_chunked("LARGE.BIN", 1003, 32768, 4096, NULL), "LARGE.BIN survives remount", &pass, &fail);
  check(verify_pattern_file_chunked("REFILL.BIN", 3000, 12000, 777, NULL), "REFILL.BIN survives remount", &pass, &fail);

  printf("\n--- Phase 7: Full Disk Scan ---\n");
  diskdump_stats_t dump = run_diskdump(true);
  check(dump.total_valid == FLOPPY_TRACKS * 2 * SECTORS_PER_TRACK, "all sectors readable", &pass, &fail);
  check(dump.total_invalid == 0, "no unreadable sectors", &pass, &fail);
  check(dump.retries.failed == 0, "no final read failures", &pass, &fail);
  check(dump.retries.recovered <= 8, "recovered read retry threshold", &pass, &fail);

done:
  if (mounted) {
    f12_unmount(&fs);
    mounted = false;
  }
  floppy_motor_off(&floppy);
  floppy_select(&floppy, false);

  printf("\n=== Test-Full Complete ===\n");
  printf("  Checks: %d passed, %d failed\n", pass, fail);
  printf("  Result: %s\n", fail == 0 ? "ALL PASSED" : "SOME FAILED");
}

static void cmd_selftest(int argc, char **argv) {
  (void)argc; (void)argv;

  printf("This will FORMAT the disk and run full write/read/verify.\n");
  printf("Continue? [y/N] ");

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

  int pass = 0, fail = 0;

  printf("\n--- Phase 1: Mount Existing Disk ---\n");
  f12_err_t err = do_mount();
  if (err == F12_OK) {
    printf("  Existing disk mounted, listing files:\n");
    f12_dir_t dir;
    f12_stat_t st;
    f12_opendir(&fs, "/", &dir);
    int count = 0;
    while (f12_readdir(&dir, &st) == F12_OK) {
      printf("    %-12s %8lu\n", st.name, st.size);
      count++;
    }
    f12_closedir(&dir);
    printf("  %d files found\n", count);
    f12_unmount(&fs);
  } else {
    printf("  No existing filesystem (%s)\n", f12_strerror(err));
  }

  printf("\n--- Phase 2: Format ---\n");
  setup_io();
  err = f12_format(&fs, "SELFTEST", false);
  check(err == F12_OK, "format quick", &pass, &fail);

  err = do_mount();
  check(err == F12_OK, "mount after format", &pass, &fail);
  if (err != F12_OK) return;
  mounted = true;

  #define NUM_TEST_FILES 10
  struct { const char *name; uint32_t size; } tests[NUM_TEST_FILES] = {
    {"TINY.BIN",   1},
    {"SMALL.DAT",  100},
    {"HALF.DAT",   256},
    {"SECT.DAT",   512},
    {"MULTI.DAT",  1024},
    {"MED.DAT",    4096},
    {"BIG.DAT",    10000},
    {"LARGE.DAT",  20000},
    {"HUGE.DAT",   35000},
    {"MAX.DAT",    50000},
  };

  printf("\n--- Phase 3: Write %d Test Files ---\n", NUM_TEST_FILES);
  uint32_t write_bytes = 0;
  uint32_t write_start = to_ms_since_boot(get_absolute_time());
  for (int i = 0; i < NUM_TEST_FILES; i++) {
    fill_pattern(self_buf, i, tests[i].size);
    f12_file_t *f = f12_open(&fs, tests[i].name, "w");
    if (!f) {
      printf("  FAIL: open %s for write: %s\n", tests[i].name, f12_strerror(f12_errno(&fs)));
      fail++;
      continue;
    }
    uint32_t wrote = f12_write_full(f, self_buf, tests[i].size);
    f12_err_t cerr = f12_close(f);
    if (wrote != tests[i].size || cerr != F12_OK) {
      printf("  FAIL: %s wrote %lu/%lu close=%s\n",
             tests[i].name, wrote, tests[i].size, f12_strerror(cerr));
      fail++;
      continue;
    }
    write_bytes += tests[i].size;
    printf("  wrote %s (%lu bytes)\n", tests[i].name, tests[i].size);
  }
  uint32_t write_ms = to_ms_since_boot(get_absolute_time()) - write_start;
  uint32_t write_bps = write_ms ? (write_bytes * 1000) / write_ms : 0;
  printf("  Write: %lu bytes in %lu ms = %lu B/s (%lu kbit/s)\n",
         write_bytes, write_ms, write_bps, (write_bps * 8) / 1000);

  printf("\n--- Phase 4: Read Back & Verify ---\n");
  uint32_t read_bytes = 0;
  uint32_t read_start = to_ms_since_boot(get_absolute_time());
  for (int i = 0; i < NUM_TEST_FILES; i++) {
    f12_file_t *f = f12_open(&fs, tests[i].name, "r");
    if (!f) {
      printf("  FAIL: open %s for read\n", tests[i].name);
      fail++;
      continue;
    }
    uint32_t got = f12_read_full(f, self_buf, tests[i].size);
    f12_close(f);
    read_bytes += got;

    f12_stat_t st;
    f12_stat(&fs, tests[i].name, &st);

    bool size_ok = (got == tests[i].size && st.size == tests[i].size);
    bool cksum_ok = (checksum_buf(self_buf, got) == pattern_checksum(i, tests[i].size));

    char tag[64];
    snprintf(tag, sizeof(tag), "%s size=%lu cksum=0x%08lX",
             tests[i].name, got, checksum_buf(self_buf, got));
    check(size_ok && cksum_ok, tag, &pass, &fail);
  }
  uint32_t read_ms = to_ms_since_boot(get_absolute_time()) - read_start;
  uint32_t read_bps = read_ms ? (read_bytes * 1000) / read_ms : 0;
  printf("  Read: %lu bytes in %lu ms = %lu B/s (%lu kbit/s)\n",
         read_bytes, read_ms, read_bps, (read_bps * 8) / 1000);

  printf("\n--- Phase 5: Delete 5 Files ---\n");
  for (int i = 0; i < 5; i++) {
    err = f12_delete(&fs, tests[i].name);
    char tag[32];
    snprintf(tag, sizeof(tag), "delete %s", tests[i].name);
    check(err == F12_OK, tag, &pass, &fail);
  }
  for (int i = 0; i < 5; i++) {
    f12_stat_t st;
    err = f12_stat(&fs, tests[i].name, &st);
    char tag[32];
    snprintf(tag, sizeof(tag), "%s gone", tests[i].name);
    check(err == F12_ERR_NOT_FOUND, tag, &pass, &fail);
  }

  printf("\n--- Phase 6: Write 5 New Files in Freed Space ---\n");
  struct { const char *name; uint32_t size; } new_files[5] = {
    {"NEW01.DAT", 500},
    {"NEW02.DAT", 2048},
    {"NEW03.DAT", 8000},
    {"NEW04.DAT", 15000},
    {"NEW05.DAT", 30000},
  };
  for (int i = 0; i < 5; i++) {
    fill_pattern(self_buf, 100 + i, new_files[i].size);
    f12_file_t *f = f12_open(&fs, new_files[i].name, "w");
    if (!f) {
      printf("  FAIL: open %s for write\n", new_files[i].name);
      fail++;
      continue;
    }
    uint32_t wrote = f12_write_full(f, self_buf, new_files[i].size);
    f12_err_t cerr = f12_close(f);
    if (wrote != new_files[i].size || cerr != F12_OK) {
      printf("  FAIL: %s wrote %lu/%lu close=%s\n",
             new_files[i].name, wrote, new_files[i].size, f12_strerror(cerr));
      fail++;
      continue;
    }
    printf("  wrote %s (%lu bytes)\n", new_files[i].name, new_files[i].size);
  }

  printf("\n--- Phase 7: Verify ALL Remaining Files ---\n");
  for (int i = 5; i < NUM_TEST_FILES; i++) {
    f12_file_t *f = f12_open(&fs, tests[i].name, "r");
    if (!f) {
      printf("  FAIL: open %s\n", tests[i].name);
      fail++;
      continue;
    }
    uint32_t got = f12_read_full(f, self_buf, tests[i].size);
    f12_close(f);
    bool ok = (got == tests[i].size) &&
              (checksum_buf(self_buf, got) == pattern_checksum(i, tests[i].size));
    char tag[64];
    snprintf(tag, sizeof(tag), "original %s verified", tests[i].name);
    check(ok, tag, &pass, &fail);
  }
  for (int i = 0; i < 5; i++) {
    f12_file_t *f = f12_open(&fs, new_files[i].name, "r");
    if (!f) {
      printf("  FAIL: open %s\n", new_files[i].name);
      fail++;
      continue;
    }
    uint32_t got = f12_read_full(f, self_buf, new_files[i].size);
    f12_close(f);
    bool ok = (got == new_files[i].size) &&
              (checksum_buf(self_buf, got) == pattern_checksum(100 + i, new_files[i].size));
    char tag[64];
    snprintf(tag, sizeof(tag), "new %s verified", new_files[i].name);
    check(ok, tag, &pass, &fail);
  }

  printf("\n--- Phase 8: Read All 2880 Sectors ---\n");
  int valid_sectors = 0;
  int invalid_sectors = 0;
  sector_t sector;
  uint32_t scan_start = to_ms_since_boot(get_absolute_time());

  for (int track = 0; track < FLOPPY_TRACKS; track++) {
    for (int side = 0; side < 2; side++) {
      int track_valid = 0;
      for (int s = 1; s <= SECTORS_PER_TRACK; s++) {
        sector.track = track;
        sector.side = side;
        sector.sector_n = s;
        sector.valid = false;
        floppy_status_t st = floppy_read_sector(&floppy, &sector);
        if (st == FLOPPY_OK && sector.valid) {
          valid_sectors++;
          track_valid++;
        } else {
          invalid_sectors++;
        }
      }
      if (track_valid < SECTORS_PER_TRACK)
        printf("  T%02d/S%d: %d/%d sectors\n", track, side,
               track_valid, SECTORS_PER_TRACK);
    }
    if ((track + 1) % 10 == 0)
      printf("  ... %d tracks done\n", track + 1);
  }
  uint32_t scan_ms = to_ms_since_boot(get_absolute_time()) - scan_start;
  uint32_t scan_bytes = (uint32_t)valid_sectors * SECTOR_SIZE;
  uint32_t scan_bps = scan_ms ? (scan_bytes * 1000) / scan_ms : 0;
  printf("  Valid: %d  Invalid: %d  Total: %d\n",
         valid_sectors, invalid_sectors, valid_sectors + invalid_sectors);
  printf("  Scan: %lu bytes in %lu.%01lus = %lu B/s (%lu kbit/s)\n",
         scan_bytes, scan_ms / 1000, (scan_ms % 1000) / 100,
         scan_bps, (scan_bps * 8) / 1000);
  printf("  Industry ref: 500 kbit/s raw, ~62.5 KB/s user data (single sector)\n");
  printf("  Theoretical max: ~45 KB/s sequential (seek + rotational latency)\n");
  check(valid_sectors == 2880, "all 2880 sectors readable", &pass, &fail);

  f12_unmount(&fs);
  mounted = false;
  #undef NUM_TEST_FILES

  printf("\n=== Throughput Summary ===\n");
  printf("  File write:  %lu B/s (%lu kbit/s)\n", write_bps, (write_bps * 8) / 1000);
  printf("  File read:   %lu B/s (%lu kbit/s)\n", read_bps, (read_bps * 8) / 1000);
  printf("  Full scan:   %lu B/s (%lu kbit/s)\n", scan_bps, (scan_bps * 8) / 1000);
  printf("  HD raw rate: 62500 B/s (500 kbit/s)\n");

  printf("\n  Results: %d passed, %d failed -- %s\n",
         pass, fail, fail == 0 ? "ALL PASSED" : "SOME FAILED");
}

static void cmd_selftest2(int argc, char **argv) {
  if (argc < 3) {
    printf("Usage: selftest2 <iterations> <filesize>\n");
    printf("  selftest2 30 1024    30 rounds of 1KB files\n");
    printf("  selftest2 10 50000   10 rounds of 50KB files\n");
    return;
  }

  int iterations = atoi(argv[1]);
  int filesize = atoi(argv[2]);

  if (iterations < 1 || iterations > 10000) {
    printf("Iterations must be 1-10000\n");
    return;
  }
  if (filesize < 1 || filesize > SELF_BUF_SIZE) {
    printf("File size must be 1-%d\n", SELF_BUF_SIZE);
    return;
  }

  printf("Stress test: %d iterations, %d byte files\n", iterations, filesize);
  printf("This will FORMAT the disk. Continue? [y/N] ");

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
  f12_err_t err = f12_format(&fs, "STRESS", false);
  if (err != F12_OK) {
    printf("Format failed: %s\n", f12_strerror(err));
    return;
  }
  err = do_mount();
  if (err != F12_OK) {
    printf("Mount failed: %s\n", f12_strerror(err));
    return;
  }
  mounted = true;
  printf("  Formatted and mounted\n");

  struct { const char *name; uint32_t size; int id; } anchors[] = {
    {"ANCHOR1.DAT", 512, 9000},
    {"ANCHOR2.DAT", 4096, 9001},
    {"ANCHOR3.DAT", 10000, 9002},
  };
  int num_anchors = 3;

  printf("\n--- Anchor Files ---\n");
  for (int i = 0; i < num_anchors; i++) {
    fill_pattern(self_buf, anchors[i].id, anchors[i].size);
    f12_file_t *f = f12_open(&fs, anchors[i].name, "w");
    if (!f) {
      printf("  FATAL: cannot write %s\n", anchors[i].name);
      return;
    }
    uint32_t wrote = f12_write_full(f, self_buf, anchors[i].size);
    f12_err_t cerr = f12_close(f);
    if (wrote != anchors[i].size || cerr != F12_OK) {
      printf("  FATAL: %s write failed\n", anchors[i].name);
      return;
    }
    printf("  %s (%lu bytes)\n", anchors[i].name, anchors[i].size);
  }

  uint16_t free_cl = count_free_clusters();
  uint32_t free_bytes = (uint32_t)free_cl * 512;
  int files_per_round = (free_bytes * 8 / 10) / filesize;
  if (files_per_round > 200) files_per_round = 200;
  if (files_per_round < 1) files_per_round = 1;

  printf("  Free: %lu bytes, %d files per round\n\n", free_bytes, files_per_round);

  int total_pass = 0, total_fail = 0;
  uint32_t total_written = 0, total_verified = 0;
  uint32_t test_start = to_ms_since_boot(get_absolute_time());

  for (int iter = 0; iter < iterations; iter++) {
    printf("--- Round %d/%d ---\n", iter + 1, iterations);

    int written_count = 0;
    uint32_t round_start = to_ms_since_boot(get_absolute_time());

    for (int i = 0; i < files_per_round; i++) {
      char name[13];
      snprintf(name, sizeof(name), "T%03d.DAT", i);
      int file_id = iter * 1000 + i;

      fill_pattern(self_buf, file_id, filesize);
      f12_file_t *f = f12_open(&fs, name, "w");
      if (!f) break;

      uint32_t wrote = f12_write_full(f, self_buf, filesize);
      f12_err_t cerr = f12_close(f);
      if (wrote != (uint32_t)filesize || cerr != F12_OK) {
        printf("  write error: %s wrote %lu/%d close=%s\n",
               name, wrote, filesize, f12_strerror(cerr));
        break;
      }
      written_count++;
      total_written += filesize;
    }

    uint32_t write_ms = to_ms_since_boot(get_absolute_time()) - round_start;

    f12_unmount(&fs);
    mounted = false;
    err = do_mount();
    if (err != F12_OK) {
      printf("  FATAL: remount failed: %s\n", f12_strerror(err));
      total_fail++;
      break;
    }
    mounted = true;

    int iter_fail = 0;

    for (int i = 0; i < written_count; i++) {
      char name[13];
      snprintf(name, sizeof(name), "T%03d.DAT", i);
      int file_id = iter * 1000 + i;

      f12_file_t *f = f12_open(&fs, name, "r");
      if (!f) { iter_fail++; printf("  FAIL: open %s\n", name); continue; }

      uint32_t got = f12_read_full(f, self_buf, filesize);
      f12_close(f);
      total_verified += got;

      if (got != (uint32_t)filesize ||
          checksum_buf(self_buf, got) != pattern_checksum(file_id, filesize)) {
        printf("  FAIL: %s cksum mismatch\n", name);
        iter_fail++;
      }
    }

    for (int i = 0; i < num_anchors; i++) {
      f12_file_t *f = f12_open(&fs, anchors[i].name, "r");
      if (!f) { iter_fail++; printf("  FAIL: anchor %s missing\n", anchors[i].name); continue; }

      uint32_t got = f12_read_full(f, self_buf, anchors[i].size);
      f12_close(f);

      if (got != anchors[i].size ||
          checksum_buf(self_buf, got) != pattern_checksum(anchors[i].id, anchors[i].size)) {
        printf("  FAIL: anchor %s corrupted\n", anchors[i].name);
        iter_fail++;
      }
    }

    for (int i = 0; i < written_count; i++) {
      char name[13];
      snprintf(name, sizeof(name), "T%03d.DAT", i);
      f12_delete(&fs, name);
    }

    if (iter_fail == 0) {
      total_pass++;
      printf("  PASS  %d files + %d anchors  write=%lu.%01lus\n",
             written_count, num_anchors, write_ms / 1000, (write_ms % 1000) / 100);
    } else {
      total_fail++;
      printf("  FAIL  %d errors\n", iter_fail);
    }
  }

  uint32_t test_ms = to_ms_since_boot(get_absolute_time()) - test_start;

  printf("\n=== Stress Test Complete ===\n");
  printf("  Rounds:   %d passed, %d failed\n", total_pass, total_fail);
  printf("  Written:  %lu bytes total\n", total_written);
  printf("  Verified: %lu bytes total\n", total_verified);
  printf("  Duration: %lu.%01lus\n", test_ms / 1000, (test_ms % 1000) / 100);
  printf("  Result:   %s\n", total_fail == 0 ? "ALL PASSED" : "SOME FAILED");

  f12_unmount(&fs);
  mounted = false;
}

static void cmd_selftest3(int argc, char **argv) {
  int rounds = 6;
  if (argc > 2) {
    printf("Usage: selftest3 [rounds]\n");
    printf("  selftest3      default 6 full-disk overwrite rounds\n");
    printf("  selftest3 12   run 12 overwrite/remount rounds\n");
    return;
  }
  if (argc == 2) {
    rounds = atoi(argv[1]);
    if (rounds < 1 || rounds > 50) {
      printf("Rounds must be 1-50\n");
      return;
    }
  }

  printf("This will FORMAT the disk and run patch-specific overwrite/buffer tests.\n");
  printf("It fills the disk, overwrites a file with 0 clusters free, then grows/shrinks it.\n");
  printf("Continue? [y/N] ");

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
  f12_err_t err = f12_format(&fs, "PATCH3", false);
  if (err != F12_OK) {
    printf("Format failed: %s\n", f12_strerror(err));
    return;
  }

  err = do_mount();
  if (err != F12_OK) {
    printf("Mount failed: %s\n", f12_strerror(err));
    return;
  }
  mounted = true;

  const char *byte_name = "BYTEIO.BIN";
  const char *target_name = "TARGET.BIN";
  const char *filler_name = "FILLER.BIN";
  const int byte_id = 3000;
  const int target_initial_id = 3100;
  const int filler_id = 3200;
  const int target_grow_id = 3300;
  const int target_shrink_id = 3301;

  uint32_t cluster_size = fs_cluster_size();
  uint32_t byte_size = cluster_size * 8;
  uint32_t target_size = cluster_size * 8;
  uint32_t grow_size = cluster_size * 48;
  uint32_t shrink_size = cluster_size * 3;

  if (byte_size < 2048) byte_size = 2048;
  if (byte_size > 8192) byte_size = 8192;
  if (byte_size > SELF_BUF_SIZE) byte_size = SELF_BUF_SIZE;

  if (target_size < 1024) target_size = 1024;
  if (grow_size <= target_size) grow_size = target_size + cluster_size * 4;
  if (shrink_size == 0 || shrink_size >= grow_size) shrink_size = cluster_size;

  printf("  Cluster size: %lu bytes\n", cluster_size);
  printf("  Byte-I/O file: %lu bytes\n", byte_size);
  printf("  Target file: %lu -> %lu -> %lu bytes\n", target_size, grow_size, shrink_size);

  int pass = 0;
  int fail = 0;
  uint32_t written = 0;
  uint32_t read_back = 0;
  f12_err_t close_err = F12_OK;
  uint32_t t0, elapsed_ms;
  char tag[96];

  printf("\n--- Phase 1: Buffered Byte-at-a-Time I/O ---\n");
  t0 = to_ms_since_boot(get_absolute_time());
  bool ok = write_pattern_file_chunked(byte_name, byte_id, byte_size, 1, &written, &close_err);
  elapsed_ms = to_ms_since_boot(get_absolute_time()) - t0;
  snprintf(tag, sizeof(tag), "%s write 1-byte chunks", byte_name);
  check(ok && written == byte_size && close_err == F12_OK, tag, &pass, &fail);
  printf("  write: %lu bytes in %lu ms = %lu B/s\n",
         written, elapsed_ms, elapsed_ms ? (written * 1000) / elapsed_ms : 0);

  f12_stat_t st;
  err = f12_stat(&fs, byte_name, &st);
  snprintf(tag, sizeof(tag), "%s size %lu", byte_name, byte_size);
  check(err == F12_OK && st.size == byte_size, tag, &pass, &fail);

  t0 = to_ms_since_boot(get_absolute_time());
  ok = verify_pattern_file_chunked(byte_name, byte_id, byte_size, 1, &read_back);
  elapsed_ms = to_ms_since_boot(get_absolute_time()) - t0;
  snprintf(tag, sizeof(tag), "%s read 1-byte chunks", byte_name);
  check(ok && read_back == byte_size, tag, &pass, &fail);
  printf("  read:  %lu bytes in %lu ms = %lu B/s\n",
         read_back, elapsed_ms, elapsed_ms ? (read_back * 1000) / elapsed_ms : 0);

  f12_unmount(&fs);
  mounted = false;
  err = do_mount();
  check(err == F12_OK, "remount after byte I/O phase", &pass, &fail);
  if (err != F12_OK) goto done;
  mounted = true;

  printf("\n--- Phase 2: Full-Disk Same-Size Overwrites ---\n");
  ok = write_pattern_file_chunked(target_name, target_initial_id, target_size, 37, &written, &close_err);
  check(ok && written == target_size && close_err == F12_OK,
        "TARGET.BIN initial write", &pass, &fail);

  uint16_t free_before_fill = count_free_clusters();
  printf("  Free before filler: %u clusters (%lu bytes)\n",
         free_before_fill, (uint32_t)free_before_fill * cluster_size);
  check(free_before_fill > 0, "space remains for filler", &pass, &fail);

  uint32_t filler_size = (uint32_t)free_before_fill * cluster_size;
  ok = write_pattern_file_chunked(filler_name, filler_id, filler_size, 4096, &written, &close_err);
  check(ok && written == filler_size && close_err == F12_OK,
        "FILLER.BIN consumes remaining space", &pass, &fail);
  check(count_free_clusters() == 0, "disk reports 0 free clusters", &pass, &fail);
  check(verify_pattern_windows(filler_name, filler_id, filler_size),
        "FILLER.BIN spot-check before overwrite rounds", &pass, &fail);

  for (int round = 0; round < rounds; round++) {
    int round_id = target_initial_id + 1 + round;

    ok = write_pattern_file_chunked(target_name, round_id, target_size, 37, &written, &close_err);
    snprintf(tag, sizeof(tag), "round %d same-size overwrite", round + 1);
    check(ok && written == target_size && close_err == F12_OK, tag, &pass, &fail);

    f12_unmount(&fs);
    mounted = false;
    err = do_mount();
    snprintf(tag, sizeof(tag), "round %d remount", round + 1);
    check(err == F12_OK, tag, &pass, &fail);
    if (err != F12_OK) goto done;
    mounted = true;

    snprintf(tag, sizeof(tag), "round %d target verify", round + 1);
    check(verify_pattern_file_chunked(target_name, round_id, target_size, 113, NULL),
          tag, &pass, &fail);

    snprintf(tag, sizeof(tag), "round %d filler spot-check", round + 1);
    check(verify_pattern_windows(filler_name, filler_id, filler_size),
          tag, &pass, &fail);

    snprintf(tag, sizeof(tag), "round %d disk still full", round + 1);
    check(count_free_clusters() == 0, tag, &pass, &fail);
  }

  check(verify_pattern_file_chunked(filler_name, filler_id, filler_size, 4096, NULL),
        "FILLER.BIN full verify after overwrite rounds", &pass, &fail);

  printf("\n--- Phase 3: Grow/Shrink Overwrites With Free Space ---\n");
  err = f12_delete(&fs, filler_name);
  check(err == F12_OK, "delete FILLER.BIN", &pass, &fail);
  err = f12_stat(&fs, filler_name, &st);
  check(err == F12_ERR_NOT_FOUND, "FILLER.BIN removed", &pass, &fail);

  uint16_t free_before_grow = count_free_clusters();
  printf("  Free after filler delete: %u clusters (%lu bytes)\n",
         free_before_grow, (uint32_t)free_before_grow * cluster_size);
  check(free_before_grow > 0, "space returned after filler delete", &pass, &fail);

  ok = write_pattern_file_chunked(target_name, target_grow_id, grow_size, 73, &written, &close_err);
  check(ok && written == grow_size && close_err == F12_OK,
        "TARGET.BIN grow overwrite", &pass, &fail);

  f12_unmount(&fs);
  mounted = false;
  err = do_mount();
  check(err == F12_OK, "remount after grow overwrite", &pass, &fail);
  if (err != F12_OK) goto done;
  mounted = true;

  err = f12_stat(&fs, target_name, &st);
  check(err == F12_OK && st.size == grow_size,
        "TARGET.BIN grow size after remount", &pass, &fail);
  check(verify_pattern_file_chunked(target_name, target_grow_id, grow_size, 257, NULL),
        "TARGET.BIN grow content verify", &pass, &fail);

  uint16_t free_after_grow = count_free_clusters();
  int32_t expected_free_after_grow =
      (int32_t)free_before_grow + (int32_t)clusters_for_size(target_size) - (int32_t)clusters_for_size(grow_size);
  snprintf(tag, sizeof(tag), "free clusters after grow = %ld", (long)expected_free_after_grow);
  check(expected_free_after_grow >= 0 &&
        free_after_grow == (uint16_t)expected_free_after_grow, tag, &pass, &fail);

  ok = write_pattern_file_chunked(target_name, target_shrink_id, shrink_size, 19, &written, &close_err);
  check(ok && written == shrink_size && close_err == F12_OK,
        "TARGET.BIN shrink overwrite", &pass, &fail);

  f12_unmount(&fs);
  mounted = false;
  err = do_mount();
  check(err == F12_OK, "remount after shrink overwrite", &pass, &fail);
  if (err != F12_OK) goto done;
  mounted = true;

  err = f12_stat(&fs, target_name, &st);
  check(err == F12_OK && st.size == shrink_size,
        "TARGET.BIN shrink size after remount", &pass, &fail);
  check(verify_pattern_file_chunked(target_name, target_shrink_id, shrink_size, 97, NULL),
        "TARGET.BIN shrink content verify", &pass, &fail);

  uint16_t free_after_shrink = count_free_clusters();
  int32_t expected_free_after_shrink =
      (int32_t)free_after_grow + (int32_t)clusters_for_size(grow_size) - (int32_t)clusters_for_size(shrink_size);
  snprintf(tag, sizeof(tag), "free clusters after shrink = %ld", (long)expected_free_after_shrink);
  check(expected_free_after_shrink >= 0 &&
        free_after_shrink == (uint16_t)expected_free_after_shrink, tag, &pass, &fail);

  check(verify_pattern_file_chunked(byte_name, byte_id, byte_size, 1, NULL),
        "BYTEIO.BIN still intact at end", &pass, &fail);

done:
  if (mounted) {
    f12_unmount(&fs);
    mounted = false;
  }

  printf("\n=== Selftest3 Complete ===\n");
  printf("  Checks: %d passed, %d failed\n", pass, fail);
  printf("  Result: %s\n", fail == 0 ? "ALL PASSED" : "SOME FAILED");
}

static void cmd_selftest3_a(int argc, char **argv) {
  int rounds = 6;
  if (argc > 2) {
    printf("Usage: selftest3-a [rounds]\n");
    printf("  selftest3-a      format + prepare state for reboot-resume test\n");
    printf("  selftest3-a 10   do 10 full-disk overwrite rounds before reboot\n");
    return;
  }
  if (argc == 2) {
    rounds = atoi(argv[1]);
    if (rounds < 1 || rounds > 50) {
      printf("Rounds must be 1-50\n");
      return;
    }
  }

  printf("This will FORMAT the disk and prepare a reboot-resume patch test.\n");
  printf("It leaves the disk full and writes a state marker for selftest3-b.\n");
  printf("Continue? [y/N] ");

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
  f12_err_t err = f12_format(&fs, "P3A", false);
  if (err != F12_OK) {
    printf("Format failed: %s\n", f12_strerror(err));
    return;
  }

  err = do_mount();
  if (err != F12_OK) {
    printf("Mount failed: %s\n", f12_strerror(err));
    return;
  }
  mounted = true;

  const int byte_id = 4300;
  const int target_initial_id = 4400;
  const int filler_id = 4500;

  uint32_t cluster_size = fs_cluster_size();
  uint32_t byte_size = cluster_size * 8;
  uint32_t target_size = cluster_size * 8;
  if (byte_size < 2048) byte_size = 2048;
  if (byte_size > 8192) byte_size = 8192;
  if (byte_size > SELF_BUF_SIZE) byte_size = SELF_BUF_SIZE;
  if (target_size < 1024) target_size = 1024;

  int pass = 0;
  int fail = 0;
  uint32_t written = 0;
  uint32_t read_back = 0;
  f12_err_t close_err = F12_OK;
  char tag[96];

  selftest3_state_t state;
  memset(&state, 0, sizeof(state));
  memcpy(state.magic, SELFTEST3_STATE_MAGIC, sizeof(state.magic));
  state.version = SELFTEST3_STATE_VERSION;
  state.stage = SELFTEST3_STAGE_A_READY;
  state.seed = SELFTEST3_RANDOM_SEED;
  state.byte_id = byte_id;
  state.byte_size = byte_size;
  state.target_id = target_initial_id + rounds;
  state.target_size = target_size;
  state.filler_id = filler_id;
  state.filler_size = 0;
  state.cluster_size = cluster_size;
  state.rounds_a = (uint32_t)rounds;

  printf("  Cluster size: %lu bytes\n", cluster_size);
  printf("  Byte-I/O file: %lu bytes\n", byte_size);
  printf("  Target file: %lu bytes, %d rounds before reboot\n", target_size, rounds);

  printf("\n--- Phase A1: Buffered Byte-at-a-Time I/O ---\n");
  bool ok = write_pattern_file_chunked(SELFTEST3_BYTE_FILE, byte_id, byte_size, 1, &written, &close_err);
  check(ok && written == byte_size && close_err == F12_OK,
        "BYTEIO.BIN write 1-byte chunks", &pass, &fail);
  check(verify_pattern_file_chunked(SELFTEST3_BYTE_FILE, byte_id, byte_size, 1, &read_back) &&
        read_back == byte_size,
        "BYTEIO.BIN read 1-byte chunks", &pass, &fail);

  printf("\n--- Phase A2: Build Full-Disk Resume State ---\n");
  ok = write_pattern_file_chunked(SELFTEST3_TARGET_FILE, target_initial_id, target_size, 37, &written, &close_err);
  check(ok && written == target_size && close_err == F12_OK,
        "TARGET.BIN initial write", &pass, &fail);

  check(selftest3_store_state(&state), "SELF3.STA placeholder write", &pass, &fail);

  uint16_t free_before_fill = count_free_clusters();
  uint32_t filler_size = (uint32_t)free_before_fill * cluster_size;
  state.filler_size = filler_size;
  printf("  Free before filler: %u clusters (%lu bytes)\n",
         free_before_fill, filler_size);
  check(free_before_fill > 0, "space remains for filler", &pass, &fail);

  ok = write_pattern_file_chunked(SELFTEST3_FILLER_FILE, filler_id, filler_size, 4096, &written, &close_err);
  check(ok && written == filler_size && close_err == F12_OK,
        "FILLER.BIN consumes remaining space", &pass, &fail);
  check(count_free_clusters() == 0, "disk reports 0 free clusters", &pass, &fail);
  check(verify_pattern_windows(SELFTEST3_FILLER_FILE, filler_id, filler_size),
        "FILLER.BIN spot-check before reboot", &pass, &fail);

  int current_target_id = target_initial_id;
  for (int round = 0; round < rounds; round++) {
    current_target_id++;

    ok = write_pattern_file_chunked(SELFTEST3_TARGET_FILE, current_target_id, target_size, 37,
                                    &written, &close_err);
    snprintf(tag, sizeof(tag), "round %d same-size overwrite", round + 1);
    check(ok && written == target_size && close_err == F12_OK, tag, &pass, &fail);

    err = selftest3_remount_after_pause(0);
    snprintf(tag, sizeof(tag), "round %d remount", round + 1);
    check(err == F12_OK, tag, &pass, &fail);
    if (err != F12_OK) goto done_a;

    snprintf(tag, sizeof(tag), "round %d target verify", round + 1);
    check(verify_pattern_file_chunked(SELFTEST3_TARGET_FILE, current_target_id, target_size, 113, NULL),
          tag, &pass, &fail);

    snprintf(tag, sizeof(tag), "round %d filler spot-check", round + 1);
    check(verify_pattern_windows(SELFTEST3_FILLER_FILE, filler_id, filler_size),
          tag, &pass, &fail);
  }

  state.target_id = (uint32_t)current_target_id;
  check(selftest3_store_state(&state), "SELF3.STA final write on full disk", &pass, &fail);

  selftest3_state_t verify_state;
  bool state_ok = selftest3_load_state(&verify_state) &&
                  verify_state.target_id == state.target_id &&
                  verify_state.filler_size == state.filler_size &&
                  verify_state.stage == SELFTEST3_STAGE_A_READY;
  check(state_ok, "SELF3.STA verify after final write", &pass, &fail);

done_a:
  if (mounted) {
    f12_unmount(&fs);
    mounted = false;
  }
  floppy_motor_off(&floppy);
  floppy_select(&floppy, false);

  printf("\n--- Powercycle ---\n");
  printf("  Now reboot or power-cycle the Pico.\n");
  printf("  Then run: selftest3-b\n");
  printf("  Optional longer soak: selftest3-b 12\n");
  printf("  The disk should still contain %s, %s, %s, %s.\n",
         SELFTEST3_BYTE_FILE, SELFTEST3_TARGET_FILE,
         SELFTEST3_FILLER_FILE, SELFTEST3_STATE_FILE);

  printf("\n=== Selftest3-A Complete ===\n");
  printf("  Checks: %d passed, %d failed\n", pass, fail);
  printf("  Result: %s\n", fail == 0 ? "READY FOR POWER CYCLE" : "FAILED");
}

static void cmd_selftest3_b(int argc, char **argv) {
  int rounds = 12;
  if (argc > 2) {
    printf("Usage: selftest3-b [rounds]\n");
    printf("  selftest3-b      resume after reboot with 12 randomized overwrite rounds\n");
    printf("  selftest3-b 20   longer randomized overwrite/remount soak\n");
    return;
  }
  if (argc == 2) {
    rounds = atoi(argv[1]);
    if (rounds < 1 || rounds > 100) {
      printf("Rounds must be 1-100\n");
      return;
    }
  }

  if (mounted) {
    f12_unmount(&fs);
    mounted = false;
  }

  f12_err_t err = do_mount();
  if (err != F12_OK) {
    printf("Mount failed: %s\n", f12_strerror(err));
    printf("Run selftest3-a first on a sacrificial disk.\n");
    return;
  }
  mounted = true;

  selftest3_state_t state;
  if (!selftest3_load_state(&state)) {
    printf("State file %s is missing or invalid.\n", SELFTEST3_STATE_FILE);
    printf("Run selftest3-a first, then reboot, then run selftest3-b.\n");
    goto done_b;
  }

  if (state.stage != SELFTEST3_STAGE_A_READY) {
    printf("State file is not in the expected pre-reboot phase.\n");
    printf("Current stage: %lu\n", state.stage);
    goto done_b;
  }

  printf("Resuming patch test without format.\n");
  printf("  Cluster size: %lu bytes\n", state.cluster_size);
  printf("  Target from phase A: id=%lu size=%lu\n", state.target_id, state.target_size);
  printf("  Filler size: %lu bytes\n", state.filler_size);
  printf("  Randomized same-size rounds: %d\n", rounds);

  int pass = 0;
  int fail = 0;
  char tag[96];

  check(fs_cluster_size() == state.cluster_size, "cluster size matches saved state", &pass, &fail);

  printf("\n--- Phase B1: Verify Post-Reboot State ---\n");
  check(verify_pattern_file_chunked(SELFTEST3_BYTE_FILE, (int)state.byte_id, state.byte_size, 1, NULL),
        "BYTEIO.BIN survived reboot", &pass, &fail);
  check(verify_pattern_file_chunked(SELFTEST3_TARGET_FILE, (int)state.target_id, state.target_size, 113, NULL),
        "TARGET.BIN survived reboot", &pass, &fail);
  check(verify_pattern_windows(SELFTEST3_FILLER_FILE, (int)state.filler_id, state.filler_size),
        "FILLER.BIN spot-check after reboot", &pass, &fail);
  check(count_free_clusters() == 0, "disk still full after reboot", &pass, &fail);

  printf("\n--- Phase B2: Randomized Full-Disk Overwrites ---\n");
  uint32_t rng = state.seed ^ state.target_id ^ ((uint32_t)rounds * 0x9E3779B9u);
  uint32_t current_target_id = state.target_id;
  uint32_t current_target_size = state.target_size;
  uint32_t current_target_clusters = clusters_for_size(current_target_size);

  for (int round = 0; round < rounds; round++) {
    uint32_t write_chunk = selftest3_random_chunk(&rng);
    uint32_t verify_chunk = selftest3_random_chunk(&rng);
    uint32_t pause_ms = selftest3_random_pause_ms(&rng);
    uint32_t written = 0;
    f12_err_t close_err = F12_OK;

    current_target_id++;
    bool ok = write_pattern_file_chunked(SELFTEST3_TARGET_FILE, (int)current_target_id,
                                         current_target_size, write_chunk,
                                         &written, &close_err);
    snprintf(tag, sizeof(tag), "round %d overwrite chunk=%lu", round + 1, write_chunk);
    check(ok && written == current_target_size && close_err == F12_OK, tag, &pass, &fail);

    err = selftest3_remount_after_pause(pause_ms);
    snprintf(tag, sizeof(tag), "round %d remount after %lums idle", round + 1, pause_ms);
    check(err == F12_OK, tag, &pass, &fail);
    if (err != F12_OK) goto done_b_counts;

    snprintf(tag, sizeof(tag), "round %d target verify chunk=%lu", round + 1, verify_chunk);
    check(verify_pattern_file_chunked(SELFTEST3_TARGET_FILE, (int)current_target_id,
                                      current_target_size, verify_chunk, NULL),
          tag, &pass, &fail);

    if (((round + 1) % 4) == 0) {
      snprintf(tag, sizeof(tag), "round %d filler full verify", round + 1);
      check(verify_pattern_file_chunked(SELFTEST3_FILLER_FILE, (int)state.filler_id,
                                        state.filler_size, 4096, NULL),
            tag, &pass, &fail);
    } else {
      snprintf(tag, sizeof(tag), "round %d filler spot-check", round + 1);
      check(verify_pattern_windows(SELFTEST3_FILLER_FILE, (int)state.filler_id, state.filler_size),
            tag, &pass, &fail);
    }

    snprintf(tag, sizeof(tag), "round %d disk still full", round + 1);
    check(count_free_clusters() == 0, tag, &pass, &fail);

    if (round == 0 || round == rounds - 1) {
      selftest3_state_t verify_state;
      snprintf(tag, sizeof(tag), "round %d state file readable", round + 1);
      check(selftest3_load_state(&verify_state) && verify_state.stage == SELFTEST3_STAGE_A_READY,
            tag, &pass, &fail);
    }
  }

  printf("\n--- Phase B3: Randomized Grow/Shrink Overwrites ---\n");
  err = f12_delete(&fs, SELFTEST3_FILLER_FILE);
  check(err == F12_OK, "delete FILLER.BIN", &pass, &fail);

  f12_stat_t st;
  err = f12_stat(&fs, SELFTEST3_FILLER_FILE, &st);
  check(err == F12_ERR_NOT_FOUND, "FILLER.BIN removed", &pass, &fail);

  uint32_t free_clusters = count_free_clusters();
  printf("  Free after filler delete: %lu clusters (%lu bytes)\n",
         free_clusters, free_clusters * state.cluster_size);
  check(free_clusters > 0, "space returned after filler delete", &pass, &fail);

  int size_rounds = rounds / 2 + 4;
  for (int round = 0; round < size_rounds; round++) {
    uint32_t max_target_clusters = free_clusters + current_target_clusters;
    if (max_target_clusters > 96) max_target_clusters = 96;
    if (max_target_clusters == 0) max_target_clusters = 1;

    uint32_t new_size = selftest3_random_target_size(&rng, state.cluster_size,
                                                     max_target_clusters,
                                                     current_target_clusters);
    uint32_t new_clusters = clusters_for_size(new_size);
    uint32_t write_chunk = selftest3_random_chunk(&rng);
    uint32_t pause_ms = selftest3_random_pause_ms(&rng);
    uint32_t written = 0;
    f12_err_t close_err = F12_OK;
    int32_t expected_free;

    current_target_id++;
    bool ok = write_pattern_file_chunked(SELFTEST3_TARGET_FILE, (int)current_target_id, new_size,
                                         write_chunk, &written, &close_err);
    snprintf(tag, sizeof(tag), "size round %d write %lu bytes chunk=%lu",
             round + 1, new_size, write_chunk);
    check(ok && written == new_size && close_err == F12_OK, tag, &pass, &fail);

    err = selftest3_remount_after_pause(pause_ms);
    snprintf(tag, sizeof(tag), "size round %d remount after %lums idle", round + 1, pause_ms);
    check(err == F12_OK, tag, &pass, &fail);
    if (err != F12_OK) goto done_b_counts;

    snprintf(tag, sizeof(tag), "size round %d content verify", round + 1);
    check(verify_pattern_file_chunked(SELFTEST3_TARGET_FILE, (int)current_target_id,
                                      new_size, selftest3_random_chunk(&rng), NULL),
          tag, &pass, &fail);

    expected_free = (int32_t)free_clusters + (int32_t)current_target_clusters - (int32_t)new_clusters;
    free_clusters = count_free_clusters();
    snprintf(tag, sizeof(tag), "size round %d free clusters = %ld", round + 1, (long)expected_free);
    check(expected_free >= 0 && free_clusters == (uint32_t)expected_free, tag, &pass, &fail);

    if (((round + 1) % 3) == 0) {
      snprintf(tag, sizeof(tag), "size round %d BYTEIO.BIN verify", round + 1);
      check(verify_pattern_file_chunked(SELFTEST3_BYTE_FILE, (int)state.byte_id, state.byte_size, 1, NULL),
            tag, &pass, &fail);
    }

    current_target_size = new_size;
    current_target_clusters = new_clusters;
  }

  state.stage = SELFTEST3_STAGE_B_DONE;
  state.seed = rng;
  state.target_id = current_target_id;
  state.target_size = current_target_size;
  state.filler_size = 0;
  check(selftest3_store_state(&state), "SELF3.STA update after phase B", &pass, &fail);

  selftest3_state_t final_state;
  bool final_state_ok = selftest3_load_state(&final_state) &&
                        final_state.stage == SELFTEST3_STAGE_B_DONE &&
                        final_state.target_id == state.target_id &&
                        final_state.target_size == state.target_size;
  check(final_state_ok, "SELF3.STA verify after phase B", &pass, &fail);

  check(verify_pattern_file_chunked(SELFTEST3_BYTE_FILE, (int)state.byte_id, state.byte_size, 1, NULL),
        "BYTEIO.BIN final verify", &pass, &fail);

done_b_counts:
  printf("\n=== Selftest3-B Complete ===\n");
  printf("  Checks: %d passed, %d failed\n", pass, fail);
  printf("  Result: %s\n", fail == 0 ? "ALL PASSED" : "SOME FAILED");

done_b:
  if (mounted) {
    f12_unmount(&fs);
    mounted = false;
  }
  floppy_motor_off(&floppy);
  floppy_select(&floppy, false);
}

static void floppy_play_note(uint16_t freq, uint16_t ms) {
  if (freq == 0) {
    sleep_ms(ms);
    return;
  }
  uint32_t period = 1000000 / freq;
  uint32_t end = to_ms_since_boot(get_absolute_time()) + ms;
  uint8_t pos = floppy.track;
  bool inward = (pos < 40);

  while (to_ms_since_boot(get_absolute_time()) < end) {
    if (pos >= 78) inward = false;
    if (pos <= 1) inward = true;

    gpio_put_oc(floppy.pins.direction, inward ? 0 : 1);
    gpio_put_oc(floppy.pins.step, 0);
    sleep_us(1);
    gpio_put_oc(floppy.pins.step, 1);

    if (inward) pos++; else pos--;

    uint32_t delay = period > 5 ? period - 5 : 1;
    sleep_us(delay);
  }
  floppy.track = pos;
}

static const struct { uint16_t freq; uint16_t ms; } imperial_march[] = {
  {392, 550}, {0, 30}, {392, 550}, {0, 30}, {392, 550}, {0, 30},
  {311, 412}, {466, 138}, {0, 30},
  {392, 550}, {0, 30},
  {311, 412}, {466, 138}, {0, 30},
  {392, 1100}, {0, 80},

  {587, 550}, {0, 30}, {587, 550}, {0, 30}, {587, 550}, {0, 30},
  {622, 412}, {466, 138}, {0, 30},
  {370, 550}, {0, 30},
  {311, 412}, {466, 138}, {0, 30},
  {392, 1100}, {0, 80},

  {784, 550}, {0, 30}, {392, 412}, {392, 138}, {0, 30},
  {784, 550}, {0, 30},
  {740, 412}, {698, 138}, {659, 138}, {622, 138},
  {659, 275}, {0, 138},
  {415, 275}, {587, 550}, {0, 30},
  {554, 412}, {523, 138}, {466, 138}, {440, 138},
  {466, 275}, {0, 138},
  {311, 275}, {370, 550}, {0, 30},
  {311, 412}, {370, 138},
  {466, 550}, {0, 30}, {392, 412}, {466, 138},
  {587, 1100},

  {0, 0}
};

static void cmd_starwars(int argc, char **argv) {
  (void)argc; (void)argv;

  floppy_select(&floppy, true);
  floppy_motor_on(&floppy);
  floppy_seek(&floppy, 40);

  printf("  Playing Imperial March...\n");

  for (int i = 0; imperial_march[i].freq || imperial_march[i].ms; i++) {
    floppy_play_note(imperial_march[i].freq, imperial_march[i].ms);
  }

  floppy.track0_confirmed = false;
  printf("  Done.\n");
}

static diskdump_stats_t run_diskdump(bool verbose) {
  diskdump_stats_t stats;
  memset(&stats, 0, sizeof(stats));

  int total_valid = 0;
  int total_invalid = 0;
  uint32_t disk_checksum = 0;
  sector_t sector;

  floppy_stats_reset(&floppy);

  if (verbose) {
    printf("  %-8s %-6s %-10s %-10s\n", "TRACK", "SIDE", "DECODED", "ERRORS");
    printf("  %-8s %-6s %-10s %-10s\n", "-----", "----", "-------", "------");
  }

  for (int track = 0; track < FLOPPY_TRACKS; track++) {
    for (int side = 0; side < 2; side++) {
      int decoded = 0;
      int errors = 0;

      for (int s = 1; s <= SECTORS_PER_TRACK; s++) {
        sector.track = track;
        sector.side = side;
        sector.sector_n = s;
        sector.valid = false;

        floppy_status_t st = floppy_read_sector(&floppy, &sector);
        if (st == FLOPPY_OK && sector.valid) {
          decoded++;
          disk_checksum ^= checksum_buf(sector.data, SECTOR_SIZE);
        } else {
          errors++;
        }
      }

      total_valid += decoded;
      total_invalid += errors;

      if (verbose || decoded != SECTORS_PER_TRACK || errors != 0) {
        printf("  T%02d      %d      %2d/%-2d      %d\n",
               track, side, decoded, SECTORS_PER_TRACK, errors);
      }
    }
  }

  stats.total_valid = total_valid;
  stats.total_invalid = total_invalid;
  stats.checksum = disk_checksum;
  stats.retries = floppy_stats(&floppy);

  printf("\n  Total decoded: %d / 2880\n", total_valid);
  printf("  Errors:        %d\n", total_invalid);
  printf("  Disk checksum: 0x%08lX\n", disk_checksum);
  printf("  Read retries:  %lu attempts, %lu recovered, %lu failed\n",
         stats.retries.retries, stats.retries.recovered, stats.retries.failed);
  if (stats.retries.timeout || stats.retries.wrong_track || stats.retries.wrong_side) {
    printf("  Retry causes:  timeout=%lu wrong_track=%lu wrong_side=%lu\n",
           stats.retries.timeout, stats.retries.wrong_track, stats.retries.wrong_side);
  }

  return stats;
}

static void cmd_diskdump(int argc, char **argv) {
  bool verbose = true;
  if (argc >= 2 && strcasecmp(argv[1], "quiet") == 0) {
    verbose = false;
  }
  run_diskdump(verbose);
}

static void cmd_mfmscan(int argc, char **argv) {
  (void)argc; (void)argv;

  track_stats_t stats;

  struct { int track; int side; const char *label; } targets[] = {
    {0,  0, "Track 0 (outermost)"},
    {39, 0, "Track 39 (mid-outer)"},
    {79, 0, "Track 79 (innermost)"},
  };

  for (int t = 0; t < 3; t++) {
    printf("\n  === %s ===\n", targets[t].label);
    read_track_stats(targets[t].track, targets[t].side, &stats);

    printf("    Pulses:   %lu total\n", stats.total_pulses);
    printf("    Short:    %lu (%.1f%%)\n", stats.short_count,
           stats.total_pulses ? stats.short_count * 100.0 / stats.total_pulses : 0);
    printf("    Medium:   %lu (%.1f%%)\n", stats.medium_count,
           stats.total_pulses ? stats.medium_count * 100.0 / stats.total_pulses : 0);
    printf("    Long:     %lu (%.1f%%)\n", stats.long_count,
           stats.total_pulses ? stats.long_count * 100.0 / stats.total_pulses : 0);
    printf("    Invalid:  %lu (%.1f%%)\n", stats.invalid_count,
           stats.total_pulses ? stats.invalid_count * 100.0 / stats.total_pulses : 0);
    printf("    Syncs:    %lu\n", stats.syncs);
    printf("    Records:  %lu\n", stats.sector_records);
    printf("    Unique:   %lu / %d\n", stats.unique_sectors, SECTORS_PER_TRACK);
    printf("    CRC err:  %lu\n", stats.crc_errors);
    printf("    Adaptive: T2_max=%d  T3_max=%d\n", stats.T2_max, stats.T3_max);
    print_histogram(&stats);
  }

  printf("\n  === Per-Track Summary (side 0) ===\n");
  printf("  %-6s %-8s %-8s %-8s %-8s %-5s %-5s %-5s\n",
         "TRACK", "SHORT", "MEDIUM", "LONG", "INVALID", "UNIQ", "REC", "CRC");
  printf("  %-6s %-8s %-8s %-8s %-8s %-5s %-5s %-5s\n",
         "-----", "------", "------", "------", "-------", "----", "---", "---");

  int total_sectors = 0;
  int total_crc = 0;

  for (int track = 0; track < FLOPPY_TRACKS; track++) {
    read_track_stats(track, 0, &stats);
    printf("  T%02d    %-8lu %-8lu %-8lu %-8lu %-5lu %-5lu %-5lu\n",
           track, stats.short_count, stats.medium_count,
           stats.long_count, stats.invalid_count,
           stats.unique_sectors, stats.sector_records, stats.crc_errors);
    total_sectors += stats.unique_sectors;
    total_crc += stats.crc_errors;
  }

  printf("\n  Side 0 total: %d unique sectors decoded, %d CRC errors\n", total_sectors, total_crc);
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
#if PICO_RP2040
  // Overclock to 144MHz so PIO dividers are exact integers:
  // read PIO: 144/72 = 2.0, write PIO: 144/24 = 6.0
  // Avoids massive jitter from fractional divider with div_int=1
  set_sys_clock_khz(144000, true);
#endif
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
