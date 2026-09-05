#include "board.h"
#include "f12.h"
#include "fsck_scenario.h"
#include "mfm_probe.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#if defined(PICO_RP2040) && PICO_RP2040
#include "hardware/structs/vreg_and_chip_reset.h"
#else
#include "hardware/structs/powman.h"
#endif
#include "pico/time.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define FW_VERSION "0.4.2"
#define CMD_BUF_SIZE 256
#define IO_BUF_SIZE DISK_SECTOR_SIZE
#define WORK_BUF_SIZE (8u * DISK_SECTOR_SIZE)
#define VERIFY_CHUNK_MAX (WORK_BUF_SIZE / 2u)
#define MAX_ARGS 4
#define PULSE_BATCH 256u
#define WRITER_CLOSE_ATTEMPTS 3
#define CLUSTER_SIZE (FAT12_SECTORS_PER_CLUSTER * DISK_SECTOR_SIZE)
#define CAPACITY_FILE_COUNT 64u
#define CAPACITY_MAX_FILE_CLUSTERS 96u
#define CLI_INPUT_OVERFLOW (-1)
#define CLI_INPUT_CANCELLED (-2)
#define INPUT_POLL_US 100000
#define FAULT_MAGIC 0x46415531u

static floppy_t floppy;
static f12_t fs;
static f12_file_t owned_writer;
#ifdef FLOPPY_ALT_PINS
static const floppy_pins_t drive_pins = FLOPPY_PINS_ALT;
#else
static const floppy_pins_t drive_pins = FLOPPY_PINS_DEFAULT;
#endif

static char cmd_buf[CMD_BUF_SIZE];
static uint8_t io_buf[IO_BUF_SIZE];
static uint8_t work_buf[WORK_BUF_SIZE];
static track_t work_track;
static mfm_probe_t signal_probe;
static floppy_pulse_t pulse_buf[PULSE_BATCH];

typedef struct {
  uint32_t id;
  uint32_t size;
  uint32_t chunk;
  char prefix;
} capacity_file_t;

static capacity_file_t capacity_files[CAPACITY_FILE_COUNT];

typedef void (*cmd_fn_t)(int argc, char **argv);

typedef struct {
  const char *name;
  cmd_fn_t fn;
  bool needs_mount;
  uint8_t min_args;
  uint8_t max_args;
  const char *syntax;
  const char *desc;
} cmd_entry_t;

static void cmd_help(int argc, char **argv);
static void cmd_ls(int argc, char **argv);
static void cmd_cat(int argc, char **argv);
static void cmd_hexdump(int argc, char **argv);
static void cmd_write(int argc, char **argv);
static void cmd_commit(int argc, char **argv);
static void cmd_rm(int argc, char **argv);
static void cmd_cp(int argc, char **argv);
static void cmd_mv(int argc, char **argv);
static void cmd_stat(int argc, char **argv);
static void cmd_format(int argc, char **argv);
static void cmd_fsck(int argc, char **argv);
static void cmd_mount(int argc, char **argv);
static void cmd_unmount(int argc, char **argv);
static void cmd_status(int argc, char **argv);
static void cmd_dma(int argc, char **argv);
static void cmd_motor(int argc, char **argv);
static void cmd_select(int argc, char **argv);
static void cmd_home(int argc, char **argv);
static void cmd_pins(int argc, char **argv);
static void cmd_poll(int argc, char **argv);
static void cmd_flux(int argc, char **argv);
static void cmd_seek(int argc, char **argv);
static void cmd_dump(int argc, char **argv);
static void cmd_mfm(int argc, char **argv);
static void cmd_mfmbench(int argc, char **argv);
static void cmd_crashtest(int argc, char **argv);
static void cmd_crashcheck(int argc, char **argv);
static void cmd_rpm(int argc, char **argv);
static void cmd_test_full(int argc, char **argv);
static void cmd_test_mfm(int argc, char **argv);
static void cmd_test_control(int argc, char **argv);
static void cmd_test_media(int argc, char **argv);
static void cmd_test_fsck(int argc, char **argv);
static void cmd_diskdump(int argc, char **argv);
static void cmd_mfmscan(int argc, char **argv);
static void cmd_reboot(int argc, char **argv);
static void cmd_version(int argc, char **argv);

static const cmd_entry_t commands[] = {
    {"help", cmd_help, false, 1, 1, "", "Show all commands"},
    {"ls", cmd_ls, true, 1, 1, "", "List files"},
    {"cat", cmd_cat, true, 2, 2, "<file>", "Print file contents"},
    {"hexdump", cmd_hexdump, true, 2, 2, "<file>", "Hex dump file contents"},
    {"write", cmd_write, true, 2, 2, "<file>", "Write file"},
    {"commit", cmd_commit, false, 1, 1, "", "Retry a pending file commit"},
    {"rm", cmd_rm, true, 2, 2, "<file>", "Delete file"},
    {"cp", cmd_cp, true, 3, 3, "<src> <dst>", "Copy file"},
    {"mv", cmd_mv, true, 3, 3, "<src> <dst>", "Move or rename file"},
    {"stat", cmd_stat, true, 2, 2, "<file>", "Show file details"},
    {"format", cmd_format, false, 1, 3, "[label] [full]", "Format disk"},
    {"fsck", cmd_fsck, true, 1, 2, "[fix]", "Check or repair filesystem"},
    {"mount", cmd_mount, false, 1, 1, "", "Mount filesystem"},
    {"unmount", cmd_unmount, false, 1, 1, "", "Unmount filesystem"},
    {"status", cmd_status, false, 1, 1, "", "Show drive and disk status"},
    {"dma", cmd_dma, false, 1, 3, "[cylinder] [head]",
     "Read a track and report DMA and overrun statistics"},
    {"motor", cmd_motor, false, 2, 2, "<on|off>", "Control motor"},
    {"select", cmd_select, false, 2, 2, "<on|off>", "Control drive select"},
    {"home", cmd_home, false, 1, 1, "", "Seek to cylinder zero"},
    {"pins", cmd_pins, false, 1, 1, "", "Read GPIO states"},
    {"poll", cmd_poll, false, 1, 1, "", "Poll read-data and index"},
    {"flux", cmd_flux, false, 1, 2, "[count]", "Dump raw flux transitions"},
    {"seek", cmd_seek, false, 2, 2, "<cylinder>", "Seek the head"},
    {"dump", cmd_dump, false, 3, 4, "<cylinder> <head> [sector]",
     "Dump raw sectors"},
    {"mfm", cmd_mfm, false, 3, 3, "<cylinder> <head>", "Analyze MFM signal"},
    {"mfmbench", cmd_mfmbench, false, 1, 1, "", "Measure decoder throughput without disk I/O"},
    {"rpm", cmd_rpm, false, 1, 1, "", "Measure spindle speed"},
    {"test-full", cmd_test_full, false, 1, 2, "[rounds]",
     "Run destructive hardware tests"},
    {"test-mfm", cmd_test_mfm, false, 1, 2, "[rounds]",
     "Run destructive MFM pattern and restart tests"},
    {"test-control", cmd_test_control, false, 1, 2, "[rounds]",
     "Test seeking and motor restarts without writing"},
    {"test-media", cmd_test_media, true, 1, 1, "",
     "Run interactive media-change tests"},
    {"test-fsck", cmd_test_fsck, false, 1, 1, "",
     "Run destructive filesystem repair tests"},
    {"crashtest", cmd_crashtest, false, 1, 1, "",
     "Prepare and loop for power-cut testing"},
    {"crashcheck", cmd_crashcheck, false, 1, 1, "", "Verify after a power cut"},
    {"diskdump", cmd_diskdump, false, 1, 2, "[quiet]", "Scan every sector"},
    {"mfmscan", cmd_mfmscan, false, 1, 1, "", "Scan MFM quality"},
    {"reboot", cmd_reboot, false, 1, 1, "", "Safely reboot"},
    {"version", cmd_version, false, 1, 1, "", "Show firmware version"},
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

static void __attribute__((noreturn, used)) fault_record(const uint32_t *frame) {
  watchdog_hw->scratch[0] = FAULT_MAGIC;
  watchdog_hw->scratch[1] = frame[6];
  watchdog_hw->scratch[2] = frame[5];
  watchdog_hw->scratch[3] = frame[7];
  watchdog_reboot(0, 0, 10);
  for (;;) tight_loop_contents();
}

void __attribute__((naked)) isr_hardfault(void) {
  __asm volatile(
      "mrs r0, msp\n"
      "ldr r1, 1f\n"
      "bx r1\n"
      ".align 2\n"
      "1: .word fault_record\n");
}

static void print_reset_cause(void) {
  printf("  reset:    ");
#if defined(PICO_RP2040) && PICO_RP2040
  uint32_t reset = vreg_and_chip_reset_hw->chip_reset;
  if (reset & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS) printf(" power-on");
  if (reset & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS) printf(" run-pin");
  if (reset & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_PSM_RESTART_BITS) printf(" debug");
#else
  uint32_t reset = powman_hw->chip_reset;
  if (reset & POWMAN_CHIP_RESET_HAD_POR_BITS) printf(" power-on");
  if (reset & POWMAN_CHIP_RESET_HAD_BOR_BITS) printf(" brownout");
  if (reset & POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS) printf(" run-pin");
  if (reset & POWMAN_CHIP_RESET_HAD_DP_RESET_REQ_BITS) printf(" debug");
  if (reset & POWMAN_CHIP_RESET_HAD_RESCUE_BITS) printf(" rescue");
  if (reset & POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS) printf(" glitch");
  if (reset & POWMAN_CHIP_RESET_HAD_HZD_SYS_RESET_REQ_BITS) printf(" hazard");
  if (reset & POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS) printf(" core-power");
  if (reset & (POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_RSM_BITS |
               POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS |
               POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_BITS |
               POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_ASYNC_BITS)) {
    printf(" watchdog");
  }
#endif
  if (watchdog_caused_reboot()) printf(" watchdog-reboot");
  printf("\n");
  if (watchdog_caused_reboot() && watchdog_hw->scratch[0] == FAULT_MAGIC) {
    printf("  HARD FAULT in previous run: pc=%08lx lr=%08lx xpsr=%08lx\n",
           (unsigned long)watchdog_hw->scratch[1],
           (unsigned long)watchdog_hw->scratch[2],
           (unsigned long)watchdog_hw->scratch[3]);
    watchdog_hw->scratch[0] = 0;
  }
}

static disk_err_t do_mount(void) { return f12_mount(&fs); }

static bool writer_handle_gone(disk_err_t error) {
  return error == DISK_ERR_BAD_HANDLE || error == DISK_ERR_MEDIA_CHANGED ||
         error == DISK_ERR_NOT_MOUNTED || error == DISK_ERR_NOT_INITIALIZED;
}

static void writer_forget(void) {
  memset(&owned_writer, 0, sizeof(owned_writer));
}

static disk_err_t writer_commit(void) {
  if (owned_writer.token.id == 0) return DISK_OK;
  disk_err_t error = DISK_OK;
  for (unsigned attempt = 0; attempt < WRITER_CLOSE_ATTEMPTS; attempt++) {
    error = f12_close(&owned_writer);
    if (error == DISK_OK) {
      writer_forget();
      return DISK_OK;
    }
    if (writer_handle_gone(error)) {
      writer_forget();
      return error;
    }
  }
  return error;
}

static disk_err_t writer_abort(void) {
  if (owned_writer.token.id == 0) return DISK_OK;
  disk_err_t error = f12_abort(&owned_writer);
  if (error == DISK_OK || writer_handle_gone(error)) writer_forget();
  return error;
}

static disk_err_t writer_open(const char *name) {
  disk_err_t error = writer_commit();
  if (error != DISK_OK) return error;
  error = f12_open(&fs, name, F12_OPEN_WRITE, &owned_writer);
  return error;
}

static disk_err_t unmount_filesystem(void) {
  disk_err_t error = writer_commit();
  if (error != DISK_OK) return error;
  bool mounted;
  error = f12_is_mounted(&fs, &mounted);
  if (error != DISK_OK) return error;
  return mounted ? f12_unmount(&fs) : DISK_OK;
}

static disk_result_t f12_write_full(f12_file_t *file, const void *buf,
                                    uint32_t len) {
  disk_result_t total = {.error = DISK_OK, .count = 0};
  while (total.count < len) {
    uint32_t chunk = len - (uint32_t)total.count;
    if (chunk > DISK_SECTOR_SIZE) chunk = DISK_SECTOR_SIZE;
    disk_result_t result =
        f12_write(file, (const uint8_t *)buf + total.count, chunk);
    if (result.count > chunk) {
      total.error = DISK_ERR_IO;
      return total;
    }
    total.count += result.count;
    if (result.error != DISK_OK) {
      total.error = result.error;
      return total;
    }
    if (result.count == 0) {
      total.error = DISK_ERR_IO;
      return total;
    }
  }
  return total;
}

static disk_result_t f12_read_full(f12_file_t *file, void *buf,
                                   uint32_t capacity) {
  disk_result_t total = {.error = DISK_OK, .count = 0};
  while (total.count < capacity) {
    size_t remaining = (size_t)capacity - total.count;
    size_t chunk = remaining < DISK_SECTOR_SIZE ? remaining : DISK_SECTOR_SIZE;
    disk_result_t result = f12_read(file, (uint8_t *)buf + total.count, chunk);
    if (result.count > chunk) {
      total.error = DISK_ERR_IO;
      return total;
    }
    total.count += result.count;
    if (result.error == DISK_END) return total;
    if (result.error != DISK_OK) {
      total.error = result.error;
      return total;
    }
    if (result.count == 0) {
      total.error = DISK_ERR_IO;
      return total;
    }
  }
  return total;
}

static unsigned char ascii_fold(unsigned char value) {
  if (value >= (unsigned char)'A' && value <= (unsigned char)'Z') {
    return (unsigned char)(value + ((unsigned char)'a' - (unsigned char)'A'));
  }
  return value;
}

static bool text_equal_case(const char *left, const char *right) {
  if (!left || !right) return false;
  while (*left != '\0' && *right != '\0') {
    if (ascii_fold((unsigned char)*left) != ascii_fold((unsigned char)*right)) {
      return false;
    }
    left++;
    right++;
  }
  return *left == *right;
}

static bool canonical_name(const char *input, char output[13]) {
  fat12_name_t name;
  if (fat12_name_parse(input, &name) != DISK_OK) return false;
  size_t pos = 0;
  for (size_t i = 0; i < sizeof(name.name) && name.name[i] != ' '; i++) {
    output[pos++] = name.name[i];
  }
  size_t ext = 0;
  while (ext < sizeof(name.ext) && name.ext[ext] != ' ') ext++;
  if (ext != 0) {
    output[pos++] = '.';
    memcpy(output + pos, name.ext, ext);
    pos += ext;
  }
  output[pos] = '\0';
  return text_equal_case(input, output);
}

static bool resolve_name(const char *input, char output[13]) {
  if (canonical_name(input, output)) return true;
  printf("Invalid 8.3 filename: %s\n", input);
  return false;
}

static void print_prompt(void) {
  bool mounted;
  disk_err_t error = f12_is_mounted(&fs, &mounted);
  if (error != DISK_OK) printf("[??]> ");
  else if (mounted) printf("[A:]> ");
  else printf("[--]> ");
}

static int cli_readline(char buf[static CMD_BUF_SIZE]) {
  size_t pos = 0;
  bool overflow = false;
  memset(buf, 0, CMD_BUF_SIZE);

  for (;;) {
    int c = getchar_timeout_us(INPUT_POLL_US);
    if (c == PICO_ERROR_TIMEOUT) {
      floppy_poll(&floppy);
      continue;
    }

    if (c == '\r' || c == '\n') {
      printf("\r\n");
      if (overflow) {
        buf[0] = '\0';
        return CLI_INPUT_OVERFLOW;
      }
      buf[pos] = '\0';
      return (int)pos;
    }

    if (c == 3) {
      printf("^C\r\n");
      buf[0] = '\0';
      return CLI_INPUT_CANCELLED;
    }

    if (c == 21) {
      while (pos > 0) {
        printf("\b \b");
        pos--;
      }
      overflow = false;
      continue;
    }

    if (c == 8 || c == 127) {
      if (!overflow && pos > 0) {
        printf("\b \b");
        pos--;
      }
      continue;
    }

    if (c >= 32 && c < 127) {
      if (pos < CMD_BUF_SIZE - 1u) {
        buf[pos++] = (char)c;
        putchar(c);
      } else {
        overflow = true;
      }
    }
  }
}

static bool cli_confirm(void) {
  char line[CMD_BUF_SIZE];
  int response = cli_readline(line);
  if (response == CLI_INPUT_OVERFLOW) {
    printf("Confirmation too long; cancelled.\n");
    return false;
  }
  if (response == CLI_INPUT_CANCELLED || !text_equal_case(line, "y")) {
    printf("Cancelled.\n");
    return false;
  }
  return true;
}

static int tokenize(char *buf, char **argv, int max_args) {
  int argc = 0;
  char *p = buf;
  while (*p) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') break;
    if (argc == max_args) return -1;
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = '\0';
  }
  return argc;
}

static bool parse_u32(const char *text, uint32_t minimum, uint32_t maximum,
                      uint32_t *value) {
  if (!text || !*text || !value || minimum > maximum) return false;
  uint32_t parsed = 0;
  for (const unsigned char *cursor = (const unsigned char *)text;
       *cursor != '\0'; cursor++) {
    if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9') return false;
    uint32_t digit = (uint32_t)(*cursor - (unsigned char)'0');
    if (parsed > maximum / 10u ||
        (parsed == maximum / 10u && digit > maximum % 10u)) {
      return false;
    }
    parsed = parsed * 10u + digit;
  }
  if (parsed < minimum) return false;
  *value = parsed;
  return true;
}

static const cmd_entry_t *find_command(const char *name) {
  for (unsigned i = 0; i < NUM_COMMANDS; i++) {
    if (text_equal_case(name, commands[i].name)) return &commands[i];
  }
  return NULL;
}

static void print_usage(const cmd_entry_t *command) {
  printf("Usage: %s%s%s\n", command->name,
         command->syntax[0] == '\0' ? "" : " ", command->syntax);
}

static void hexdump_row(uint32_t offset, const uint8_t *data, size_t count) {
  printf("  %08lX: ", (unsigned long)offset);
  for (size_t i = 0; i < 16u; i++) {
    if (i < count) printf("%02X ", (unsigned)data[i]);
    else printf("   ");
    if (i == 7) printf(" ");
  }
  printf(" |");
  for (size_t i = 0; i < count; i++) {
    putchar((data[i] >= 32 && data[i] < 127) ? data[i] : '.');
  }
  printf("|\n");
}

typedef struct {
  int total_valid;
  int total_invalid;
  uint32_t checksum;
  floppy_stats_t retries;
} diskdump_stats_t;

static diskdump_stats_t run_diskdump(bool verbose);

static bool drive_status(const char *operation, disk_err_t status) {
  if (status == DISK_OK) return true;
  printf("%s failed: %s\n", operation, disk_strerror(status));
  return false;
}

static bool drive_idle(void) {
  bool idle = drive_status("Motor stop", floppy_motor_off(&floppy));
  if (!drive_status("Drive deselect", floppy_select(&floppy, false))) idle = false;
  return idle;
}

static bool drive_generation(uint32_t *generation) {
  return drive_status("Media generation",
                      floppy_media_generation(&floppy, generation));
}

static disk_err_t read_track(uint8_t cylinder, uint8_t head, uint32_t generation) {
  work_track.cylinder = cylinder;
  work_track.head = head;
  return floppy_read_track(&floppy, generation, &work_track);
}

static unsigned track_sectors(const track_t *track) {
  unsigned count = 0;
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    if (track_has(track, sector)) count++;
  }
  return count;
}

static void print_drive_stats(const floppy_stats_t *s) {
  printf("  Track reads:   %lu\n", s->reads);
  printf("  Flux words:    %lu\n", s->flux_words);
  printf("  Track writes:  %lu (DMA)\n", s->dma_writes);
  printf("  Read retries:  %lu attempts, %lu recovered, %lu failed\n",
         s->retries, s->recovered, s->failed);
  printf("  Read faults:   timeout=%lu crc=%lu track=%lu side=%lu\n",
         s->timeout, s->crc, s->wrong_track, s->wrong_side);
  printf("  Flow faults:   overrun=%lu underrun=%lu\n", s->overruns,
         s->underruns);
  printf("  Media changes: %lu\n", s->media_changes);
  printf("  Ring peak:     %lu/%u words\n", s->ring_peak,
         FLOPPY_FLUX_RING_WORDS);
}

static void cmd_dma(int argc, char **argv) {
  uint32_t cylinder = 0;
  uint32_t head = 0;
  if (argc >= 2 && !parse_u32(argv[1], 0, DISK_CYLINDERS - 1u, &cylinder)) {
    printf("Cylinder must be 0 through %u.\n", DISK_CYLINDERS - 1u);
    return;
  }
  if (argc >= 3 && !parse_u32(argv[2], 0, DISK_HEADS - 1u, &head)) {
    printf("Head must be 0 through %u.\n", DISK_HEADS - 1u);
    return;
  }
  if (!drive_status("Statistics reset", floppy_stats_reset(&floppy))) return;
  uint32_t generation;
  if (!drive_generation(&generation)) return;
  disk_err_t status = read_track((uint8_t)cylinder, (uint8_t)head, generation);
  printf("  Track C%luH%lu read: %s\n", (unsigned long)cylinder,
         (unsigned long)head, disk_strerror(status));
  printf("  Sectors:       %u/%u recovered\n", track_sectors(&work_track),
         DISK_SECTORS_PER_TRACK);
  floppy_stats_t stats;
  if (drive_status("Statistics", floppy_stats(&floppy, &stats))) {
    print_drive_stats(&stats);
  }
  drive_idle();
}

static disk_err_t read_track_stats(uint8_t track, uint8_t side,
                                    mfm_probe_t *probe) {
  uint32_t generation;
  disk_err_t error = floppy_media_generation(&floppy, &generation);
  if (error != DISK_OK) {
    memset(probe, 0, sizeof(*probe));
    return error;
  }
  return mfm_probe_track(&floppy, generation, track, side, NULL, probe);
}

static void print_histogram(const mfm_probe_t *stats) {
  uint32_t peak = 0;
  for (unsigned i = 0; i < MFM_PROBE_BINS; i++) {
    if (stats->histogram[i] > peak) peak = stats->histogram[i];
  }
  if (peak == 0) return;

  unsigned first = 0;
  unsigned last = MFM_PROBE_BINS - 1u;
  while (first < MFM_PROBE_BINS && stats->histogram[first] == 0) first++;
  while (last > first && stats->histogram[last] == 0) last--;

  printf("  Pulse Distribution (delta ticks):\n");
  for (unsigned i = first; i <= last; i++) {
    if (stats->histogram[i] == 0) continue;
    uint32_t bar = (stats->histogram[i] * 50u) / peak;
    printf("  %3u: %6lu |", i, stats->histogram[i]);
    for (uint32_t j = 0; j < bar; j++) printf("#");
    printf("\n");
  }
}

static double percent(uint32_t part, uint32_t total) {
  return total ? part * 100.0 / total : 0.0;
}

static void print_track_stats(const mfm_probe_t *probe, const char *indent) {
  printf("%sProbe stage: %s\n", indent, mfm_probe_stage_name(probe->stage));
  mfm_probe_counts_t counts = mfm_probe_counts(probe);
  printf("%sPulses:   %lu total\n", indent, (unsigned long)counts.total);
  printf("%sShort:    %lu (%.1f%%)\n", indent, (unsigned long)counts.short_count,
         percent(counts.short_count, counts.total));
  printf("%sMedium:   %lu (%.1f%%)\n", indent, (unsigned long)counts.medium_count,
         percent(counts.medium_count, counts.total));
  printf("%sLong:     %lu (%.1f%%)\n", indent, (unsigned long)counts.long_count,
         percent(counts.long_count, counts.total));
  printf("%sInvalid:  %lu (%.1f%%)\n", indent, (unsigned long)counts.invalid_count,
         percent(counts.invalid_count, counts.total));
  printf("%sRevs:     %lu/%u complete, minimum %lu/%u sectors per revolution\n",
         indent, (unsigned long)probe->revolutions, MFM_PROBE_REVOLUTIONS,
         (unsigned long)probe->minimum_sectors, DISK_SECTORS_PER_TRACK);
  printf("%sRecords:  %lu, unique %u/%u, syncs %lu\n", indent,
         (unsigned long)probe->decoder.sectors_read,
         mfm_probe_sector_count(probe->sectors), DISK_SECTORS_PER_TRACK,
         (unsigned long)probe->decoder.syncs_found);
  printf("%sErrors:   crc=%lu format=%lu address=%lu duplicate=%lu data=%lu\n",
         indent, (unsigned long)probe->decoder.crc_errors,
         (unsigned long)probe->decoder.format_errors,
         (unsigned long)probe->wrong_address, (unsigned long)probe->duplicates,
         (unsigned long)probe->mismatches);
  printf("%sCell:     %.3f ticks, short limit %u, medium limit %u\n", indent,
         probe->decoder.cell_q8 / 256.0, mfm_short_limit(&probe->decoder),
         mfm_medium_limit(&probe->decoder));
  print_histogram(probe);
}

static uint8_t gen_pattern_byte(uint32_t file_id, uint32_t offset) {
  uint32_t v = file_id * 2654435761u + offset * 40503u;
  return (uint8_t)((v >> 16) & 0xFFu);
}

static uint32_t checksum_extend(uint32_t sum, const uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; i++) sum = (sum << 5) + sum + buf[i];
  return sum;
}

typedef void (*fill_fn_t)(const void *ctx, uint8_t *buf, uint32_t offset,
                          uint32_t size);

static void fill_pattern(const void *ctx, uint8_t *buf, uint32_t offset,
                         uint32_t size) {
  uint32_t file_id = *(const uint32_t *)ctx;
  for (uint32_t i = 0; i < size; i++) buf[i] = gen_pattern_byte(file_id, offset + i);
}

static void fill_blob(const void *ctx, uint8_t *buf, uint32_t offset,
                      uint32_t size) {
  memcpy(buf, (const uint8_t *)ctx + offset, size);
}

static bool write_file(const char *name, fill_fn_t fill, const void *ctx,
                       uint32_t size, uint32_t chunk_size, uint32_t *written_out,
                       disk_err_t *close_err_out) {
  if (chunk_size == 0 || chunk_size > WORK_BUF_SIZE) chunk_size = WORK_BUF_SIZE;
  uint32_t written = 0;
  disk_err_t error = writer_open(name);
  if (error == DISK_OK) {
    while (written < size) {
      uint32_t chunk = size - written;
      if (chunk > chunk_size) chunk = chunk_size;
      fill(ctx, work_buf, written, chunk);
      disk_result_t result = f12_write_full(&owned_writer, work_buf, chunk);
      written += (uint32_t)result.count;
      if (result.error != DISK_OK || result.count != chunk) {
        disk_err_t abort_error = writer_abort();
        error = abort_error != DISK_OK ? abort_error
                : result.error != DISK_OK ? result.error
                                          : DISK_ERR_IO;
        break;
      }
    }
    if (error == DISK_OK) error = writer_commit();
  }
  if (written_out) *written_out = written;
  if (close_err_out) *close_err_out = error;
  return error == DISK_OK;
}

static bool write_pattern_file(const char *name, uint32_t file_id, uint32_t size,
                               uint32_t chunk_size, uint32_t *written_out,
                               disk_err_t *close_err_out) {
  return write_file(name, fill_pattern, &file_id, size, chunk_size, written_out,
                    close_err_out);
}

static bool write_blob_file(const char *name, const void *buf, uint32_t size,
                            uint32_t chunk_size, uint32_t *written_out,
                            disk_err_t *close_err_out) {
  return write_file(name, fill_blob, buf, size, chunk_size, written_out,
                    close_err_out);
}

static bool verify_file(const char *name, fill_fn_t fill, const void *ctx,
                        uint32_t size, uint32_t chunk_size) {
  if (chunk_size == 0 || chunk_size > VERIFY_CHUNK_MAX) chunk_size = VERIFY_CHUNK_MAX;
  f12_stat_t stat;
  disk_err_t error = f12_stat(&fs, name, &stat);
  if (error != DISK_OK) {
    printf("  %s: stat failed: %s\n", name, disk_strerror(error));
    return false;
  }
  if (stat.size != size) {
    printf("  %s: size %lu, expected %lu\n", name, (unsigned long)stat.size,
           (unsigned long)size);
    return false;
  }
  f12_file_t file;
  error = f12_open(&fs, name, F12_OPEN_READ, &file);
  if (error != DISK_OK) {
    printf("  %s: open failed: %s\n", name, disk_strerror(error));
    return false;
  }

  bool ok = true;
  uint32_t total = 0;
  uint8_t *expected = work_buf + VERIFY_CHUNK_MAX;
  while (ok && total < size) {
    uint32_t chunk = size - total;
    if (chunk > chunk_size) chunk = chunk_size;
    disk_result_t result = f12_read(&file, work_buf, chunk);
    if (result.count > chunk || result.error != DISK_OK || result.count == 0) {
      printf("  %s: read failed at offset %lu: %s (%lu bytes)\n", name,
             (unsigned long)total, disk_strerror(result.error),
             (unsigned long)result.count);
      ok = false;
      break;
    }
    fill(ctx, expected, total, (uint32_t)result.count);
    for (size_t i = 0; i < result.count; i++) {
      if (work_buf[i] == expected[i]) continue;
      printf("  %s: mismatch at offset %lu: read %02X, expected %02X\n", name,
             (unsigned long)(total + i), (unsigned)work_buf[i],
             (unsigned)expected[i]);
      ok = false;
      break;
    }
    total += (uint32_t)result.count;
  }
  error = f12_close(&file);
  if (error != DISK_OK) {
    printf("  %s: close failed: %s\n", name, disk_strerror(error));
    ok = false;
  }
  return ok && total == size;
}

static bool verify_pattern_file(const char *name, uint32_t file_id, uint32_t size,
                                uint32_t chunk_size) {
  return verify_file(name, fill_pattern, &file_id, size, chunk_size);
}

static bool read_file_exact(const char *name, void *buf, uint32_t size) {
  f12_stat_t stat;
  if (f12_stat(&fs, name, &stat) != DISK_OK || stat.size != size) return false;
  f12_file_t file;
  if (f12_open(&fs, name, F12_OPEN_READ, &file) != DISK_OK) return false;
  disk_result_t result = f12_read_full(&file, buf, size);
  bool closed = f12_close(&file) == DISK_OK;
  return closed && result.error == DISK_OK && result.count == size;
}

static uint32_t capacity_random(uint32_t *state) {
  *state = *state * 1664525u + 1013904223u;
  return *state;
}

static uint32_t capacity_partition(uint32_t *state, uint32_t clusters,
                                   uint32_t files) {
  if (files == 1u) return clusters;
  uint32_t available = clusters - (files - 1u);
  uint32_t limit = available;
  if (limit > CAPACITY_MAX_FILE_CLUSTERS) limit = CAPACITY_MAX_FILE_CLUSTERS;
  return 1u + capacity_random(state) % limit;
}

static uint32_t capacity_cluster_count(uint32_t size) {
  return (size + CLUSTER_SIZE - 1u) / CLUSTER_SIZE;
}

static void capacity_name(const capacity_file_t *file, uint32_t number,
                          char name[13]) {
  snprintf(name, 13, "%c%07lu.BIN", file->prefix, (unsigned long)number);
}

static bool capacity_write(capacity_file_t *file, char prefix, uint32_t number,
                           uint32_t clusters, uint32_t *state) {
  file->prefix = prefix;
  file->id = capacity_random(state);
  file->size = (clusters - 1u) * CLUSTER_SIZE + 1u +
               capacity_random(state) % CLUSTER_SIZE;
  file->chunk = 1u + capacity_random(state) % WORK_BUF_SIZE;
  char name[13];
  capacity_name(file, number, name);
  uint32_t written = 0;
  disk_err_t close_error = DISK_OK;
  bool ok = write_pattern_file(name, file->id, file->size, file->chunk,
                               &written, &close_error);
  if (!ok && (owned_writer.token.id != 0)) writer_abort();
  return ok && written == file->size && close_error == DISK_OK;
}

static bool capacity_verify(void) {
  uint32_t failures = 0;
  for (uint32_t i = 0; i < CAPACITY_FILE_COUNT; i++) {
    const capacity_file_t *file = &capacity_files[i];
    char name[13];
    capacity_name(file, i, name);
    if (!verify_pattern_file(name, file->id, file->size, file->chunk)) failures++;
  }
  return failures == 0;
}

#define CRASH_STATE_FILE "CRASH.STA"
#define CRASH_STATE_MAGIC "CRASH001"
#define CRASH_STATE_VERSION 1u
#define CRASH_STABLE_FILE "STABLE.BIN"
#define CRASH_TARGET_FILE "TARGET.BIN"
#define CRASH_FILLER_FILE "FILLER.BIN"

typedef struct {
  char magic[8];
  uint32_t version;
  uint32_t stable_id;
  uint32_t stable_size;
  uint32_t target_old_id;
  uint32_t target_new_id;
  uint32_t target_size;
  uint32_t filler_id;
  uint32_t filler_size;
  uint32_t checksum;
  uint8_t reserved[DISK_SECTOR_SIZE - 8u - 9u * sizeof(uint32_t)];
} crash_state_t;

_Static_assert(sizeof(crash_state_t) == DISK_SECTOR_SIZE, "crash_state_t size");

static uint32_t crash_state_checksum(const crash_state_t *state) {
  const uint8_t *data = (const uint8_t *)state;
  size_t checksum_offset = offsetof(crash_state_t, checksum);
  uint32_t sum = 0;
  for (size_t i = 0; i < sizeof(*state); i++) {
    uint8_t value =
        i >= checksum_offset && i < checksum_offset + sizeof(state->checksum)
            ? 0
            : data[i];
    sum = (sum << 5) + sum + value;
  }
  return sum;
}

static bool crash_state_valid(const crash_state_t *state) {
  uint32_t capacity = FAT12_DATA_CLUSTERS * CLUSTER_SIZE;
  return memcmp(state->magic, CRASH_STATE_MAGIC, sizeof(state->magic)) == 0 &&
         state->version == CRASH_STATE_VERSION &&
         state->checksum == crash_state_checksum(state) &&
         state->target_old_id != state->target_new_id &&
         state->stable_size != 0 && state->stable_size <= capacity &&
         state->target_size != 0 && state->target_size <= capacity &&
         state->filler_size != 0 && state->filler_size <= capacity;
}

static bool crash_state_store(crash_state_t *state) {
  state->checksum = crash_state_checksum(state);
  uint32_t written = 0;
  disk_err_t close_err = DISK_OK;
  return write_blob_file(CRASH_STATE_FILE, state, sizeof(*state), 37, &written,
                         &close_err) &&
         written == sizeof(*state) && close_err == DISK_OK;
}

static bool crash_state_load(crash_state_t *state) {
  return read_file_exact(CRASH_STATE_FILE, state, sizeof(*state)) &&
         crash_state_valid(state);
}

static void cmd_help(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("\nCommands:\n");
  for (unsigned i = 0; i < NUM_COMMANDS; i++) {
    printf("  %-12s %-28s %s\n", commands[i].name, commands[i].syntax,
           commands[i].desc);
  }
  printf("\n");
}

static void cmd_ls(int argc, char **argv) {
  (void)argc;
  (void)argv;
  f12_dir_t dir;
  f12_stat_t st;

  disk_err_t err = f12_opendir(&fs, "/", &dir);
  if (err != DISK_OK) {
    printf("Error: %s\n", disk_strerror(err));
    return;
  }

  int count = 0;
  uint32_t total_bytes = 0;
  disk_err_t read_error;
  while ((read_error = f12_readdir(&dir, &st)) == DISK_OK) {
    if ((st.attr & FAT12_ATTR_DIRECTORY) != 0) printf("  %-12s    <DIR>\n", st.name);
    else printf("  %-12s %8lu\n", st.name, st.size);
    total_bytes += st.size;
    count++;
  }
  disk_err_t close_error = f12_closedir(&dir);
  if (read_error != DISK_END) {
    printf("Error: %s\n", disk_strerror(read_error));
    return;
  }
  if (close_error != DISK_OK) {
    printf("Error: %s\n", disk_strerror(close_error));
    return;
  }
  if (count == 0) printf("  (empty)\n");

  uint16_t free_cl;
  disk_err_t error = f12_free_count(&fs, &free_cl);
  if (error != DISK_OK) {
    printf("Free-space query failed: %s\n", disk_strerror(error));
    return;
  }
  uint32_t free_bytes = (uint32_t)free_cl * CLUSTER_SIZE;
  printf("  %d entries, %lu bytes used, %lu bytes free\n", count, total_bytes,
         free_bytes);
}

static void cmd_cat(int argc, char **argv) {
  (void)argc;
  char name[13];
  if (!resolve_name(argv[1], name)) return;

  f12_file_t file;
  disk_err_t error = f12_open(&fs, name, F12_OPEN_READ, &file);
  if (error != DISK_OK) {
    printf("Error: %s\n", disk_strerror(error));
    return;
  }

  int total = 0;
  for (;;) {
    disk_result_t result = f12_read(&file, io_buf, IO_BUF_SIZE);
    if (result.count > IO_BUF_SIZE) {
      printf("\nRead error: invalid transfer count\n");
      break;
    }
    for (size_t i = 0; i < result.count; i++) putchar(io_buf[i]);
    total += (int)result.count;
    if (result.error == DISK_END) break;
    if (result.error != DISK_OK) {
      printf("\nRead error: %s\n", disk_strerror(result.error));
      break;
    }
  }
  printf("\n(%d bytes)\n", total);
  error = f12_close(&file);
  if (error != DISK_OK) printf("Close error: %s\n", disk_strerror(error));
}

static void cmd_hexdump(int argc, char **argv) {
  (void)argc;
  char name[13];
  if (!resolve_name(argv[1], name)) return;

  f12_file_t file;
  disk_err_t error = f12_open(&fs, name, F12_OPEN_READ, &file);
  if (error != DISK_OK) {
    printf("Error: %s\n", disk_strerror(error));
    return;
  }

  uint32_t offset = 0;
  for (;;) {
    disk_result_t result = f12_read(&file, io_buf, 16);
    if (result.count > 16u) {
      printf("Read error: invalid transfer count\n");
      break;
    }
    if (result.count == 0 && result.error == DISK_END) break;
    if (result.error != DISK_OK && result.error != DISK_END) {
      printf("Read error: %s\n", disk_strerror(result.error));
      break;
    }
    hexdump_row(offset, io_buf, result.count);
    offset += (uint32_t)result.count;
    if (result.error == DISK_END) break;
  }
  printf("  %lu bytes\n", offset);
  error = f12_close(&file);
  if (error != DISK_OK) printf("Close error: %s\n", disk_strerror(error));
}

static void report_write_failure(const char *what, disk_err_t error) {
  disk_err_t abort_error = writer_abort();
  printf("%s\n", what);
  if (error != DISK_OK) printf("  cause: %s\n", disk_strerror(error));
  if (abort_error != DISK_OK) {
    printf("Abort failed and writer remains retained: %s\n",
           disk_strerror(abort_error));
  }
}

static void cmd_write(int argc, char **argv) {
  (void)argc;
  char name[13];
  if (!resolve_name(argv[1], name)) return;

  disk_err_t error = writer_open(name);
  if (error != DISK_OK) {
    printf("Error: %s\n", disk_strerror(error));
    return;
  }

  printf("Enter text (end with . on its own line):\n");
  uint32_t total = 0;
  char line[CMD_BUF_SIZE];
  for (;;) {
    int line_length = cli_readline(line);
    if (line_length == CLI_INPUT_CANCELLED) {
      report_write_failure("Write cancelled.", DISK_OK);
      return;
    }
    if (line_length == CLI_INPUT_OVERFLOW) {
      report_write_failure("Input line too long; write aborted.", DISK_OK);
      return;
    }
    if (strcmp(line, ".") == 0) break;

    uint32_t requested = (uint32_t)line_length + 1u;
    line[line_length] = '\n';
    disk_result_t result = f12_write_full(&owned_writer, line, requested);
    total += (uint32_t)result.count;
    if (result.error != DISK_OK || result.count != requested) {
      printf("Write failed after %lu bytes.\n", (unsigned long)total);
      report_write_failure("Write aborted.",
                           result.error == DISK_OK ? DISK_ERR_IO : result.error);
      return;
    }
  }

  error = writer_commit();
  if (error != DISK_OK) {
    printf("Commit failed and writer remains retained: %s\n", disk_strerror(error));
    return;
  }
  printf("Wrote %lu bytes to %s\n", (unsigned long)total, name);
}

static void cmd_commit(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (owned_writer.token.id == 0) {
    printf("No pending commit.\n");
    return;
  }
  disk_err_t error = writer_commit();
  if (error == DISK_OK) printf("Commit complete.\n");
  else printf("Commit failed and remains retained: %s\n", disk_strerror(error));
}

static void cmd_rm(int argc, char **argv) {
  (void)argc;
  char name[13];
  if (!resolve_name(argv[1], name)) return;
  disk_err_t err = f12_delete(&fs, name);
  if (err != DISK_OK) printf("Error: %s\n", disk_strerror(err));
  else printf("Deleted %s\n", name);
}

static void cmd_cp(int argc, char **argv) {
  (void)argc;
  char src[13];
  char dst[13];
  if (!canonical_name(argv[1], src) || !canonical_name(argv[2], dst)) {
    printf("Source and destination must be valid 8.3 filenames.\n");
    return;
  }

  f12_file_t reader;
  disk_err_t error = f12_open(&fs, src, F12_OPEN_READ, &reader);
  if (error != DISK_OK) {
    printf("Error opening %s: %s\n", src, disk_strerror(error));
    return;
  }

  error = writer_open(dst);
  if (error != DISK_OK) {
    printf("Error creating %s: %s\n", dst, disk_strerror(error));
    disk_err_t read_close = f12_close(&reader);
    if (read_close != DISK_OK) {
      printf("Reader close failed: %s\n", disk_strerror(read_close));
    }
    return;
  }

  uint32_t total = 0;
  disk_err_t copy_error = DISK_OK;
  for (;;) {
    disk_result_t read_result = f12_read(&reader, io_buf, IO_BUF_SIZE);
    if (read_result.count > IO_BUF_SIZE) {
      copy_error = DISK_ERR_IO;
      break;
    }
    if (read_result.count != 0) {
      disk_result_t write_result =
          f12_write_full(&owned_writer, io_buf, (uint32_t)read_result.count);
      total += (uint32_t)write_result.count;
      if (write_result.error != DISK_OK ||
          write_result.count != read_result.count) {
        copy_error =
            write_result.error == DISK_OK ? DISK_ERR_IO : write_result.error;
        break;
      }
    }
    if (read_result.error == DISK_END) break;
    if (read_result.error != DISK_OK) {
      copy_error = read_result.error;
      break;
    }
  }

  disk_err_t read_close = f12_close(&reader);
  if (copy_error == DISK_OK && read_close != DISK_OK) copy_error = read_close;
  disk_err_t write_close = copy_error == DISK_OK ? writer_commit() : writer_abort();
  if (write_close != DISK_OK) copy_error = write_close;
  if (copy_error != DISK_OK) {
    printf("Copy error after %lu bytes: %s\n", total, disk_strerror(copy_error));
    if ((owned_writer.token.id != 0)) printf("Commit retained; run 'commit' to retry.\n");
    return;
  }
  printf("Copied %lu bytes: %s -> %s\n", total, src, dst);
}

static void cmd_mv(int argc, char **argv) {
  (void)argc;
  char src[13];
  char dst[13];
  if (!canonical_name(argv[1], src) || !canonical_name(argv[2], dst)) {
    printf("Source and destination must be valid 8.3 filenames.\n");
    return;
  }
  disk_err_t err = f12_rename(&fs, src, dst);
  if (err != DISK_OK) {
    printf("Error: %s\n", disk_strerror(err));
    return;
  }
  printf("Renamed %s -> %s\n", src, dst);
}

static void cmd_stat(int argc, char **argv) {
  (void)argc;
  char name[13];
  if (!resolve_name(argv[1], name)) return;

  f12_stat_t st;
  disk_err_t err = f12_stat(&fs, name, &st);
  if (err != DISK_OK) {
    printf("Error: %s\n", disk_strerror(err));
    return;
  }

  printf("  Name:   %s\n", st.name);
  printf("  Size:   %lu bytes\n", st.size);
  printf("  Attr:   0x%02X", st.attr);
  if (st.attr & FAT12_ATTR_READ_ONLY) printf(" RO");
  if (st.attr & FAT12_ATTR_HIDDEN) printf(" HID");
  if (st.attr & FAT12_ATTR_SYSTEM) printf(" SYS");
  if (st.attr & FAT12_ATTR_VOLUME_ID) printf(" VOL");
  if (st.attr & FAT12_ATTR_DIRECTORY) printf(" DIR");
  if (st.attr & FAT12_ATTR_ARCHIVE) printf(" ARC");
  printf("\n");
}

static void format_progress(void *ctx, uint8_t cyl, uint8_t side, uint16_t done,
                            uint16_t total) {
  (void)ctx;
  if (done == 1 || done == total || (done % 4) == 0) {
    uint32_t pct = total ? (uint32_t)done * 100 / total : 100;
    printf("  formatting: %u/%u tracks (%lu%%), track %u side %u\n", done,
           total, (unsigned long)pct, cyl, side);
  }
}

static disk_err_t do_format(const char *label, bool full) {
  return f12_format(&fs, (f12_format_options_t){
                             .label = label,
                             .mode = full ? F12_FORMAT_FULL : F12_FORMAT_QUICK,
                             .progress = format_progress,
                             .progress_ctx = NULL,
                         });
}

static void cmd_format(int argc, char **argv) {
  const char *label = "PICODISK";
  bool full = false;

  if (argc == 2) {
    if (text_equal_case(argv[1], "full")) full = true;
    else label = argv[1];
  } else if (argc == 3) {
    if (!text_equal_case(argv[2], "full")) {
      printf("Format mode must be 'full'.\n");
      return;
    }
    label = argv[1];
    full = true;
  }

  printf("Format disk as \"%s\" (%s)? [y/N] ", label, full ? "full" : "quick");
  if (!cli_confirm()) return;

  disk_err_t err = unmount_filesystem();
  if (err != DISK_OK) {
    printf("Unmount failed: %s\n", disk_strerror(err));
    return;
  }

  err = do_format(label, full);
  if (err != DISK_OK) {
    printf("Format error: %s\n", disk_strerror(err));
    return;
  }
  printf("Format complete.\n");

  err = do_mount();
  if (err == DISK_OK) printf("Mounted.\n");
  else printf("Mount error: %s\n", disk_strerror(err));
}

static void print_fsck_report(const fat12_fsck_t *r) {
  printf("  Files:         %u\n", r->files);
  printf("  Directories:   %u\n", r->directories);
  printf("  Lost clusters: %u\n", r->lost_clusters);
  printf("  Crosslinked:   %u\n", r->crosslinked);
  printf("  Loops:         %u\n", r->loops);
  printf("  Broken chains: %u\n", r->broken_chains);
  printf("  Size mismatch: %u\n", r->size_mismatches);
  printf("  Truncated:     %u\n", r->truncated_files);
  printf("  Duplicate:     %u\n", r->duplicate_names);
  if (r->fat_mismatch) {
    const char *authority = r->fat_ambiguous             ? "ambiguous"
                            : r->authoritative_fat == 1u ? "FAT1"
                            : r->authoritative_fat == 2u ? "FAT2"
                                                         : "none";
    printf("  FAT copies:    mismatch, authority=%s, scores=%lu/%lu%s\n",
           authority, (unsigned long)r->fat1_score,
           (unsigned long)r->fat2_score,
           r->repaired_fat1 || r->repaired_fat2 ? ", repaired" : "");
  }
  if (r->removed_directories) printf("  Removed dirs:  %u\n", r->removed_directories);
  if (r->removed_duplicates) printf("  Removed dupes: %u\n", r->removed_duplicates);
  if (r->freed_tails) printf("  Freed tails:   %u\n", r->freed_tails);
  if (r->freed) printf("  Freed:         %u clusters\n", r->freed);
}

static void cmd_fsck(int argc, char **argv) {
  bool fix = argc == 2;
  if (fix && !text_equal_case(argv[1], "fix")) {
    printf("Repair mode must be 'fix'.\n");
    return;
  }

  fat12_fsck_t report;
  disk_err_t err = f12_fsck(&fs, &report, fix);
  if (err != DISK_OK) {
    printf("Error: %s\n", disk_strerror(err));
    return;
  }

  print_fsck_report(&report);

  if (fix) {
    printf("  Result: repaired and verified clean\n");
  } else if (!fat12_fsck_clean(&report)) {
    printf("  Result: DIRTY -- run 'fsck fix' to repair\n");
  } else {
    printf("  Result: clean\n");
  }
}

static void cmd_mount(int argc, char **argv) {
  (void)argc;
  (void)argv;
  disk_err_t err = unmount_filesystem();
  if (err != DISK_OK) {
    printf("Unmount failed: %s\n", disk_strerror(err));
    return;
  }

  err = do_mount();
  if (err != DISK_OK) {
    printf("Mount error: %s\n", disk_strerror(err));
    return;
  }
  printf("Mounted.\n");

  fat12_fsck_t report;
  disk_err_t check_error = f12_fsck(&fs, &report, false);
  if (check_error != DISK_OK) {
    printf("Filesystem check failed: %s\n", disk_strerror(check_error));
  } else if (!fat12_fsck_clean(&report)) {
    printf("WARNING: filesystem is not clean. Run 'fsck' for details.\n");
  }
}

static void cmd_unmount(int argc, char **argv) {
  (void)argc;
  (void)argv;
  bool mounted;
  disk_err_t error = f12_is_mounted(&fs, &mounted);
  if (error != DISK_OK) {
    printf("Filesystem state check failed: %s\n", disk_strerror(error));
    return;
  }
  if (!mounted) {
    if ((owned_writer.token.id != 0)) {
      error = writer_commit();
      if (error != DISK_OK) printf("Pending commit failed: %s\n", disk_strerror(error));
    }
    printf("Not mounted.\n");
    return;
  }
  error = writer_commit();
  if (error == DISK_OK) error = f12_unmount(&fs);
  if (error != DISK_OK) {
    printf("Unmount failed: %s\n", disk_strerror(error));
    return;
  }
  printf("Unmounted.\n");
}

static void cmd_status(int argc, char **argv) {
  (void)argc;
  (void)argv;
  bool write_protected;
  bool changed;
  bool at_track0;
  uint8_t cylinder;
  disk_err_t track_status = floppy_current_track(&floppy, &cylinder);
  if (!drive_status("Write-protect query",
                    floppy_write_protected(&floppy, &write_protected)) ||
      !drive_status("Media-change query",
                    floppy_disk_changed(&floppy, &changed)) ||
      (track_status != DISK_ERR_NO_TRACK0 &&
       !drive_status("Track query", track_status)) ||
      !drive_status("Track-zero query", floppy_at_track0(&floppy, &at_track0))) {
    return;
  }
  printf("  Drive:\n");
  printf("    Write protected: %s\n", write_protected ? "YES" : "no");
  printf("    Disk changed:    %s\n", changed ? "YES" : "no");
  if (track_status == DISK_OK) printf("    Current track:   %u\n", cylinder);
  else printf("    Current track:   unknown until homed\n");
  if (floppy.selected) printf("    At track 0:      %s\n", at_track0 ? "yes" : "no");
  else printf("    At track 0:      unknown while deselected\n");
  printf("    Pending commit:  %s\n", (owned_writer.token.id != 0) ? "YES" : "no");

  bool mounted;
  disk_err_t mount_error = f12_is_mounted(&fs, &mounted);
  if (mount_error != DISK_OK) {
    printf("  Filesystem state: %s\n", disk_strerror(mount_error));
    return;
  }
  if (!mounted) {
    printf("  Filesystem: not mounted\n");
    return;
  }

  printf("  Geometry:\n");
  printf("    Bytes/sector:     %u\n", DISK_SECTOR_SIZE);
  printf("    Sectors/cluster:  %u\n", FAT12_SECTORS_PER_CLUSTER);
  printf("    Reserved sectors: %u\n", FAT12_RESERVED_SECTORS);
  printf("    FATs:             %u\n", FAT12_NUM_FATS);
  printf("    Root entries:     %u\n", FAT12_ROOT_ENTRIES);
  printf("    Total sectors:    %u\n", DISK_SECTOR_COUNT);
  printf("    Media descriptor: 0x%02X\n", FAT12_MEDIA_DESCRIPTOR);
  printf("    Sectors/FAT:      %u\n", FAT12_SECTORS_PER_FAT);
  printf("    Sectors/track:    %u\n", DISK_SECTORS_PER_TRACK);
  printf("    Heads:            %u\n", DISK_HEADS);

  uint16_t free_cl;
  disk_err_t error = f12_free_count(&fs, &free_cl);
  if (error != DISK_OK) {
    printf("  Free-space query failed: %s\n", disk_strerror(error));
    return;
  }
  uint32_t free_bytes = (uint32_t)free_cl * CLUSTER_SIZE;
  printf("  Free: %lu bytes (%d clusters)\n", free_bytes, free_cl);
}

static void cmd_motor(int argc, char **argv) {
  (void)argc;
  if (text_equal_case(argv[1], "on")) {
    if (drive_status("Motor start", floppy_motor_on(&floppy))) printf("Motor ON\n");
  } else if (text_equal_case(argv[1], "off")) {
    if (drive_status("Motor stop", floppy_motor_off(&floppy))) printf("Motor off\n");
  } else {
    printf("Motor state must be 'on' or 'off'.\n");
  }
}

static void cmd_select(int argc, char **argv) {
  (void)argc;
  if (text_equal_case(argv[1], "on")) {
    if (drive_status("Drive select", floppy_select(&floppy, true))) {
      printf("Drive selected\n");
    }
  } else if (text_equal_case(argv[1], "off")) {
    if (drive_status("Drive deselect", floppy_select(&floppy, false))) {
      printf("Drive deselected\n");
    }
  } else {
    printf("Selection state must be 'on' or 'off'.\n");
  }
}

static void cmd_home(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("Seeking to track 0...\n");
  if (!drive_status("Seek", floppy_seek(&floppy, 0))) return;
  bool active;
  if (!drive_status("Track-zero query", floppy_at_track0(&floppy, &active))) return;
  printf("At track 0 (TRK0 pin: %s)\n", active ? "active" : "NOT active");
}

static void cmd_pins(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("  GPIO  Pin  Signal          State\n");
  printf("  ----  ---  ------          -----\n");

  struct {
    uint gpio;
    const char *fpin;
    const char *name;
    bool is_input;
  } pins[] = {
      {drive_pins.index, " 8", "INDEX", true},
      {drive_pins.track0, "26", "TRACK0", true},
      {drive_pins.write_protect, "28", "WRITE_PROTECT", true},
      {drive_pins.read_data, "30", "READ_DATA", true},
      {drive_pins.disk_change, "34", "DISK_CHANGE", true},
      {drive_pins.drive_select, "12", "DRIVE_SELECT", false},
      {drive_pins.motor_enable, "10", "MOTOR_ENABLE", false},
      {drive_pins.direction, "18", "DIRECTION", false},
      {drive_pins.step, "20", "STEP", false},
      {drive_pins.write_data, "22", "WRITE_DATA", false},
      {drive_pins.write_gate, "24", "WRITE_GATE", false},
      {drive_pins.side_select, "32", "SIDE_SELECT", false},
      {drive_pins.density, " 2", "DENSITY", false},
  };

  for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
    bool val = gpio_get(pins[i].gpio);
    printf("  GP%-2u  %s   %-15s %d (%s)%s\n", pins[i].gpio, pins[i].fpin,
           pins[i].name, val, val ? "HIGH" : "LOW",
           pins[i].is_input ? " <input>" : "");
  }
}

static void cmd_poll(int argc, char **argv) {
  (void)argc;
  (void)argv;

  printf("  Starting motor and selecting drive...\n");
  if (!drive_status("Drive select", floppy_select(&floppy, true))) return;
  if (!drive_status("Motor start", floppy_motor_on(&floppy))) {
    drive_status("Drive deselect", floppy_select(&floppy, false));
    return;
  }

  uint pin = drive_pins.read_data;
  uint ix_pin = drive_pins.index;
  printf("  Polling GP%u (read_data) and GP%u (index) for 2 seconds...\n", pin,
         ix_pin);

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

  printf("  read_data transitions: %d  (expect ~200k+ if disk present)\n",
         transitions);
  printf("  index transitions:     %d  (expect about 20 at 300 RPM)\n",
         ix_transitions);
  if (transitions == 0) printf("  No activity on read_data -- check wiring or disk.\n");
  drive_idle();
}

static void cmd_flux(int argc, char **argv) {
  uint32_t count = 200;
  if (argc == 2 && !parse_u32(argv[1], 1, 10000, &count)) {
    printf("Count must be an integer from 1 through 10000.\n");
    return;
  }

  printf("  Reading %lu raw flux transitions...\n", (unsigned long)count);
  printf("  read_data=GP%u  index=GP%u\n", drive_pins.read_data, drive_pins.index);

  uint32_t generation;
  if (!drive_generation(&generation) ||
      !drive_status("Flux capture", floppy_flux_begin(&floppy, generation))) {
    return;
  }

  for (uint32_t printed = 0; printed < count;) {
    size_t want = count - printed < PULSE_BATCH ? count - printed : PULSE_BATCH;
    disk_result_t result = floppy_flux_read(&floppy, pulse_buf, want);
    for (size_t i = 0; i < result.count; i++) {
      printf("  %4lu: delta=%3u  ix=%d\n", (unsigned long)(printed + i),
             pulse_buf[i].delta, pulse_buf[i].index);
    }
    printed += (uint32_t)result.count;
    if (result.error != DISK_OK) {
      printf("  Capture stopped after %lu transitions: %s.\n", (unsigned long)printed,
             disk_strerror(result.error));
      printf("  Check: disk inserted? read_data wiring? motor spinning?\n");
      printf("  Current read_data (GP%u) = %d\n", drive_pins.read_data,
             gpio_get(drive_pins.read_data));
      drive_status("Flux stop", floppy_flux_end(&floppy));
      return;
    }
  }

  if (!drive_status("Flux stop", floppy_flux_end(&floppy))) return;
  printf("  Done.\n");
}

static void cmd_seek(int argc, char **argv) {
  (void)argc;
  uint32_t cylinder;
  if (!parse_u32(argv[1], 0, DISK_CYLINDERS - 1u, &cylinder)) {
    printf("Cylinder must be 0 through %u.\n", DISK_CYLINDERS - 1u);
    return;
  }
  if (drive_status("Seek", floppy_seek(&floppy, (uint8_t)cylinder))) {
    printf("Head at cylinder %lu\n", (unsigned long)cylinder);
  }
}

static void cmd_dump(int argc, char **argv) {
  uint32_t cylinder;
  uint32_t head;
  uint32_t sec_start = 1;
  uint32_t sec_end = DISK_SECTORS_PER_TRACK;

  if (!parse_u32(argv[1], 0, DISK_CYLINDERS - 1u, &cylinder) ||
      !parse_u32(argv[2], 0, DISK_HEADS - 1u, &head)) {
    printf("Cylinder must be 0 through %u and head 0 through %u.\n",
           DISK_CYLINDERS - 1u, DISK_HEADS - 1u);
    return;
  }
  if (argc == 4) {
    if (!parse_u32(argv[3], 1, DISK_SECTORS_PER_TRACK, &sec_start)) {
      printf("Sector must be 1 through %u.\n", DISK_SECTORS_PER_TRACK);
      return;
    }
    sec_end = sec_start;
  }

  uint32_t generation;
  if (!drive_generation(&generation)) return;
  disk_err_t status = read_track((uint8_t)cylinder, (uint8_t)head, generation);
  printf("  --- C%lu/H%lu %s, %u/%u sectors ---\n", (unsigned long)cylinder,
         (unsigned long)head, disk_strerror(status), track_sectors(&work_track),
         DISK_SECTORS_PER_TRACK);
  for (uint32_t number = sec_start; number <= sec_end; number++) {
    uint8_t sector = (uint8_t)(number - 1u);
    if (!track_has(&work_track, sector)) {
      printf("  --- S%lu missing ---\n", (unsigned long)number);
      continue;
    }
    printf("  --- S%lu ---\n", (unsigned long)number);
    for (uint32_t row = 0; row < DISK_SECTOR_SIZE; row += 16u) {
      hexdump_row(row, work_track.data[sector] + row, 16u);
    }
  }
}

static void cmd_mfm(int argc, char **argv) {
  (void)argc;
  uint32_t cylinder;
  uint32_t head;
  if (!parse_u32(argv[1], 0, DISK_CYLINDERS - 1u, &cylinder) ||
      !parse_u32(argv[2], 0, DISK_HEADS - 1u, &head)) {
    printf("Cylinder must be 0 through %u and head 0 through %u.\n",
           DISK_CYLINDERS - 1u, DISK_HEADS - 1u);
    return;
  }

  printf("  Analyzing cylinder %lu head %lu...\n", (unsigned long)cylinder,
         (unsigned long)head);
  drive_status("MFM capture",
               read_track_stats((uint8_t)cylinder, (uint8_t)head, &signal_probe));
  print_track_stats(&signal_probe, "  ");
  drive_idle();
}

static void cmd_mfmbench(int argc, char **argv) {
  (void)argc;
  (void)argv;
  const unsigned repetitions = 64;
  printf("  Decoder benchmark at %lu MHz; no disk I/O\n",
         (unsigned long)(clock_get_hz(clk_sys) / 1000000u));
  printf("  Load is decoder time / encoded flux duration; excludes driver and IRQ work.\n");
  for (unsigned pattern = 0; pattern < MFM_TEST_PATTERNS; pattern++) {
    work_track.cylinder = 0;
    work_track.head = 0;
    if (!mfm_test_fill(&work_track, pattern, 0)) return;
    memcpy(io_buf, work_track.data[0], sizeof(io_buf));
    uint8_t *pulses = (uint8_t *)work_track.data;
    mfm_encode_t encoder;
    mfm_encode_init(&encoder, pulses, sizeof(work_track.data));
    mfm_encode_sector(&encoder, 0, 0, 0, io_buf);
    mfm_encode_gap(&encoder, 54);
    if (encoder.stopped) {
      printf("  Benchmark encoding failed\n");
      return;
    }
    uint32_t ticks = 0;
    for (size_t i = 0; i < encoder.pos; i++) ticks += pulses[i];
    uint32_t decoded = 0;
    bool exact = true;
    mfm_sector_t sector;
    absolute_time_t started = get_absolute_time();
    for (unsigned repetition = 0; repetition < repetitions; repetition++) {
      mfm_init(&signal_probe.decoder);
      for (size_t i = 0; i < encoder.pos; i++) {
        if (!mfm_feed(&signal_probe.decoder, pulses[i], &sector)) continue;
        decoded++;
        if (memcmp(sector.data, io_buf, sizeof(io_buf)) != 0) exact = false;
      }
      if (signal_probe.decoder.crc_errors || signal_probe.decoder.format_errors) {
        exact = false;
      }
    }
    int64_t elapsed_us = absolute_time_diff_us(started, get_absolute_time());
    if (elapsed_us <= 0) {
      printf("  Benchmark timer failed\n");
      return;
    }
    uint32_t rate = (uint32_t)((uint64_t)encoder.pos * repetitions * 1000000u /
                              (uint64_t)elapsed_us);
    double load = (double)elapsed_us * (MFM_TICK_HZ / 1000000u) * 100.0 /
                   ((double)ticks * repetitions);
    printf("  pattern=%s sectors=%lu/%u exact=%s pulses/s=%lu load=%.1f%%\n",
           mfm_test_pattern_names[pattern], (unsigned long)decoded, repetitions,
           exact && decoded == repetitions ? "yes" : "NO",
           (unsigned long)rate, load);
  }
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

static void check_status(disk_err_t status, const char *tag, int *pass, int *fail) {
  if (status == DISK_OK) {
    check(true, tag, pass, fail);
    return;
  }
  printf("  FAIL: %s: %s\n", tag, disk_strerror(status));
  (*fail)++;
}

static void mfm_health_check(uint8_t track, uint8_t side, const char *label,
                             int *pass, int *fail) {
  disk_err_t error = read_track_stats(track, side, &signal_probe);
  printf("  %s C%uH%u: %s, revs=%lu min=%lu/%u crc=%lu format=%lu\n",
         label, track, side, disk_strerror(error),
         (unsigned long)signal_probe.revolutions,
         (unsigned long)signal_probe.minimum_sectors, DISK_SECTORS_PER_TRACK,
         (unsigned long)signal_probe.decoder.crc_errors,
         (unsigned long)signal_probe.decoder.format_errors);
  bool clean = error == DISK_OK && mfm_probe_clean(&signal_probe);
  check(clean, label, pass, fail);
  if (!clean) print_track_stats(&signal_probe, "    ");
}

static bool run_mfm_patterns(uint32_t rounds, int *pass, int *fail) {
  static const uint8_t cylinders[] = {
      0, MFM_PRECOMP_START_TRACK - 1u, MFM_PRECOMP_START_TRACK,
      DISK_CYLINDERS - 1u,
  };
  uint32_t generation;
  disk_err_t error = floppy_seek(&floppy, 0);
  if (error == DISK_OK) error = floppy_media_generation(&floppy, &generation);
  check_status(error, "prepare pattern test", pass, fail);
  if (error != DISK_OK) return false;
  for (uint32_t round = 0; round < rounds; round++) {
    for (unsigned pattern = 0; pattern < MFM_TEST_PATTERNS; pattern++) {
      for (size_t c = 0; c < sizeof(cylinders); c++) {
        for (uint8_t head = 0; head < DISK_HEADS; head++) {
          work_track.cylinder = cylinders[c];
          work_track.head = head;
          if (!mfm_test_fill(&work_track, pattern, round)) {
            check(false, "prepare test pattern", pass, fail);
            return false;
          }
          const char *stage = "statistics reset";
          error = floppy_stats_reset(&floppy);
          if (error == DISK_OK) {
            stage = "write / verification";
            error = floppy_write_track(&floppy, generation, &work_track);
          }
          if (error == DISK_OK) {
            stage = "seek away";
            error = floppy_seek(&floppy, cylinders[c] == 0 ? DISK_CYLINDERS - 1u : 0);
          }
          if (error == DISK_OK) {
            stage = "motor stop";
            error = floppy_motor_off(&floppy);
          }
          if (error == DISK_OK) sleep_ms(1000);
          memset(&signal_probe, 0, sizeof(signal_probe));
          if (error == DISK_OK) {
            error = mfm_probe_track(&floppy, generation, cylinders[c], head,
                                    &work_track, &signal_probe);
            stage = mfm_probe_stage_name(signal_probe.stage);
          }
          floppy_stats_t stats = {0};
          disk_err_t stats_error = floppy_stats(&floppy, &stats);
          printf("  round=%lu pattern=%s C%uH%u: %s revs=%lu min=%lu "
                 "crc=%lu format=%lu data=%lu retries=%lu ring=%lu/%u "
                 "overrun=%lu underrun=%lu\n",
                 (unsigned long)(round + 1u), mfm_test_pattern_names[pattern],
                 cylinders[c], head, disk_strerror(error),
                 (unsigned long)signal_probe.revolutions,
                 (unsigned long)signal_probe.minimum_sectors,
                 (unsigned long)signal_probe.decoder.crc_errors,
                 (unsigned long)signal_probe.decoder.format_errors,
                 (unsigned long)signal_probe.mismatches,
                 (unsigned long)stats.retries, (unsigned long)stats.ring_peak,
                 FLOPPY_FLUX_RING_WORDS, (unsigned long)stats.overruns,
                 (unsigned long)stats.underruns);
          bool clean = error == DISK_OK && stats_error == DISK_OK &&
                       mfm_probe_clean(&signal_probe) && stats.retries == 0 &&
                       stats.overruns == 0 && stats.underruns == 0 &&
                       stats.media_changes == 0;
          check(clean, "pattern survives seek and motor restart", pass, fail);
          if (!clean) {
            printf("    Stage: %s; DISK_CHANGE GP%u=%d, MOTOR_ENABLE=%d, DRIVE_SELECT=%d\n",
                   stage, drive_pins.disk_change, gpio_get(drive_pins.disk_change),
                   gpio_get(drive_pins.motor_enable), gpio_get(drive_pins.drive_select));
            if (signal_probe.decoder.cell_q8 != 0) {
              print_track_stats(&signal_probe, "    ");
            } else {
              printf("    Post-restart capture was not reached; failure occurred during write or positioning.\n");
            }
            if (error != DISK_OK || stats_error != DISK_OK) return false;
          }
        }
      }
    }
  }
  return true;
}

static bool control_checkpoint(const char *stage, disk_err_t error,
                                uint32_t expected_generation) {
  uint32_t generation = 0;
  disk_err_t query = floppy_media_generation(&floppy, &generation);
  if (error == DISK_OK) error = query;
  if (error == DISK_OK && generation != expected_generation)
    error = DISK_ERR_MEDIA_CHANGED;
  printf("  %-24s %s; generation=%lu/%lu GP%u=%d motor=%d select=%d side=%d\n",
         stage, disk_strerror(error), (unsigned long)generation,
         (unsigned long)expected_generation, drive_pins.disk_change,
         gpio_get(drive_pins.disk_change), gpio_get(drive_pins.motor_enable),
         gpio_get(drive_pins.drive_select), gpio_get(drive_pins.side_select));
  return error == DISK_OK;
}

static bool control_watch(uint32_t generation) {
  uint32_t samples = 0;
  uint32_t low_samples = 0;
  uint32_t edges = 0;
  bool previous = gpio_get(drive_pins.disk_change);
  absolute_time_t deadline = make_timeout_time_ms(1000);
  while (!time_reached(deadline)) {
    bool high = gpio_get(drive_pins.disk_change);
    samples++;
    if (!high) low_samples++;
    if (high != previous) edges++;
    previous = high;
    if (low_samples == 1u && !high) {
      uint32_t observed;
      disk_err_t error = floppy_media_generation(&floppy, &observed);
      if (error != DISK_OK) return false;
    }
    sleep_us(50);
  }
  printf("  Motor-off GP%u: low=%lu/%lu samples, edges=%lu (50 us polling over 1 s)\n",
         drive_pins.disk_change, (unsigned long)low_samples,
         (unsigned long)samples, (unsigned long)edges);
  return control_checkpoint("motor-off observation",
                            low_samples ? DISK_ERR_MEDIA_CHANGED : DISK_OK,
                            generation);
}

static void cmd_test_control(int argc, char **argv) {
  uint32_t rounds = 10;
  if (argc == 2 && !parse_u32(argv[1], 1, 100, &rounds)) {
    printf("Rounds must be an integer from 1 through 100.\n");
    return;
  }
  if (owned_writer.token.id != 0) {
    printf("A file commit is pending; run 'commit' before this test.\n");
    return;
  }
  printf("  Control test: no disk writes. Leave the disk inserted. GPIO levels are raw: 0=LOW, 1=HIGH.\n");
  disk_err_t error = floppy_seek(&floppy, 0);
  uint32_t generation = 0;
  if (error == DISK_OK) error = floppy_media_generation(&floppy, &generation);
  bool clean = control_checkpoint("home / establish media", error, generation);
  for (uint32_t round = 0; clean && round < rounds; round++) {
    printf("  Round %lu/%lu\n", (unsigned long)(round + 1u), (unsigned long)rounds);
    for (uint8_t head = 0; clean && head < DISK_HEADS; head++) {
      clean = control_checkpoint("side select", floppy_side_select(&floppy, head), generation);
      if (!clean) break;
      clean = control_checkpoint("seek to 79", floppy_seek(&floppy, 79), generation);
      if (!clean) break;
      clean = control_checkpoint("motor stop", floppy_motor_off(&floppy), generation);
      if (!clean) break;
      clean = control_watch(generation);
      if (!clean) break;
      clean = control_checkpoint("restart / seek to 0", floppy_seek(&floppy, 0), generation);
    }
  }
  if (!drive_idle()) clean = false;
  printf("  Control test: %s\n", clean ? "ALL PASSED" : "FAILED");
}

static void cmd_test_mfm(int argc, char **argv) {
  uint32_t rounds = 1;
  if (argc == 2 && !parse_u32(argv[1], 1, 10, &rounds)) {
    printf("Rounds must be an integer from 1 through 10.\n");
    return;
  }
  printf("This DESTROYS data on cylinders 0, 39, 40, and 79 on both heads.\n");
  printf("Continue? [y/N] ");
  if (!cli_confirm()) return;
  disk_err_t error = unmount_filesystem();
  if (!drive_status("Unmount before pattern test", error)) return;
  int pass = 0;
  int fail = 0;
  run_mfm_patterns(rounds, &pass, &fail);
  if (!drive_idle()) fail++;
  printf("\n=== Test-MFM Complete ===\n");
  printf("  Checks: %d passed, %d failed\n", pass, fail);
  printf("  Result: %s\n", fail == 0 ? "ALL PASSED" : "SOME FAILED");
}

static void cmd_test_full(int argc, char **argv) {
  uint32_t rounds = 6;
  if (argc == 2 && !parse_u32(argv[1], 1, 100, &rounds)) {
    printf("Rounds must be an integer from 1 through 100.\n");
    return;
  }

  printf("This will FORMAT the disk, run diagnostics, file I/O, and a full "
         "sector scan.\n");
  printf("Continue? [y/N] ");
  if (!cli_confirm()) return;

  disk_err_t start_error = unmount_filesystem();
  if (start_error != DISK_OK) {
    printf("Cannot start test: %s\n", disk_strerror(start_error));
    return;
  }

  int pass = 0;
  int fail = 0;
  disk_err_t cleanup;

  printf("\n--- Phase 1: GPIO and Flux Sanity ---\n");
  cmd_pins(0, NULL);
  cmd_status(0, NULL);
  disk_err_t home_status = floppy_seek(&floppy, 0);
  check_status(home_status, "home and clear media latch", &pass, &fail);
  uint32_t test_generation = 0;
  disk_err_t generation_status = floppy_media_generation(&floppy, &test_generation);
  bool generation_ready = generation_status == DISK_OK;
  check_status(generation_status, "capture media generation", &pass, &fail);
  bool stats_reset = floppy_stats_reset(&floppy) == DISK_OK;
  check(stats_reset, "reset drive statistics", &pass, &fail);

  disk_err_t flux_status = generation_ready
                               ? floppy_flux_begin(&floppy, test_generation)
                               : DISK_ERR_INVALID;
  check(flux_status == DISK_OK, "raw flux session starts", &pass, &fail);
  if (flux_status == DISK_OK) {
    check(floppy_seek(&floppy, 1) == DISK_ERR_BUSY,
          "raw flux excludes head motion", &pass, &fail);
    check(floppy_deinit(&floppy) == DISK_ERR_BUSY,
          "raw flux excludes teardown", &pass, &fail);
    check(floppy_flux_end(&floppy) == DISK_OK, "raw flux session stops", &pass,
          &fail);
  }

  bool write_protected = true;
  bool protection_ok = drive_status(
      "Write-protect query", floppy_write_protected(&floppy, &write_protected));
  check(protection_ok && !write_protected, "disk is writable", &pass, &fail);
  cmd_poll(0, NULL);
  char *flux_args[] = {"flux", "50"};
  cmd_flux(2, flux_args);
  uint32_t current_generation = 0;
  generation_status = floppy_media_generation(&floppy, &current_generation);
  bool generation_stable =
      generation_status == DISK_OK && current_generation == test_generation;
  check(generation_stable, "media generation remains stable", &pass, &fail);

  if (home_status != DISK_OK || !generation_ready || !generation_stable ||
      !stats_reset || !protection_ok || write_protected) {
    goto done;
  }

  printf("\n--- MFM Patterns Before Format ---\n");
  if (!run_mfm_patterns(1, &pass, &fail)) goto done;

  printf("\n--- Phase 2: Full Format and Mount ---\n");
  disk_err_t err = do_format("TESTFULL", true);
  check_status(err, "full format", &pass, &fail);
  if (err != DISK_OK) goto done;

  err = do_mount();
  check_status(err, "mount after full format", &pass, &fail);
  if (err != DISK_OK) goto done;
  cmd_status(0, NULL);

  printf("\n--- Phase 3: MFM Signal Checks ---\n");
  for (uint8_t head = 0; head < DISK_HEADS; head++) {
    mfm_health_check(0, head, "outer", &pass, &fail);
    mfm_health_check(DISK_CYLINDERS - 1u, head, "inner", &pass, &fail);
  }
  check_status(floppy_stats_reset(&floppy), "reset statistics after signal checks",
               &pass, &fail);

  printf("\n--- Phase 4: Shell File Operations ---\n");
  const char text[] = "test-full hardware sequence\nformat mount write copy "
                      "move delete verify\n";
  uint32_t text_len = sizeof(text) - 1;
  disk_err_t open_error = writer_open("TEST.TXT");
  check(open_error == DISK_OK, "open TEST.TXT for write", &pass, &fail);
  if (open_error == DISK_OK) {
    disk_result_t write_result = f12_write_full(&owned_writer, text, text_len);
    disk_err_t close_err =
        write_result.error == DISK_OK ? writer_commit() : writer_abort();
    check(write_result.error == DISK_OK && write_result.count == text_len &&
              close_err == DISK_OK,
          "write TEST.TXT", &pass, &fail);
  }

  bool exact = read_file_exact("TEST.TXT", work_buf, text_len);
  check(exact && memcmp(work_buf, text, text_len) == 0, "read TEST.TXT", &pass,
        &fail);

  char *cp_args[] = {"cp", "TEST.TXT", "COPY.TXT"};
  cmd_cp(3, cp_args);
  exact = read_file_exact("COPY.TXT", work_buf, text_len);
  check(exact && memcmp(work_buf, text, text_len) == 0,
        "copy TEST.TXT to COPY.TXT", &pass, &fail);

  char *mv_args[] = {"mv", "COPY.TXT", "MOVED.TXT"};
  cmd_mv(3, mv_args);
  f12_stat_t st;
  check(f12_stat(&fs, "COPY.TXT", &st) == DISK_ERR_NOT_FOUND,
        "COPY.TXT removed by mv", &pass, &fail);
  exact = read_file_exact("MOVED.TXT", work_buf, text_len);
  check(exact && memcmp(work_buf, text, text_len) == 0, "MOVED.TXT verified",
        &pass, &fail);

  char *rm_args[] = {"rm", "MOVED.TXT"};
  cmd_rm(2, rm_args);
  check(f12_stat(&fs, "MOVED.TXT", &st) == DISK_ERR_NOT_FOUND, "rm MOVED.TXT",
        &pass, &fail);

  printf("\n--- Phase 5: Pattern Stress ---\n");
  struct {
    const char *name;
    uint32_t size;
    uint32_t id;
    uint32_t chunk;
  } files[] = {
      {"BYTEIO.BIN", 4096, 1000, 1},
      {"SMALL.BIN", DISK_SECTOR_SIZE, 1001, 17},
      {"MEDIUM.BIN", 8192, 1002, 257},
      {"LARGE.BIN", 32768, 1003, 4096},
  };

  for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    uint32_t written = 0;
    disk_err_t close_err = DISK_OK;
    bool ok = write_pattern_file(files[i].name, files[i].id, files[i].size,
                                 files[i].chunk, &written, &close_err);
    char tag[80];
    snprintf(tag, sizeof(tag), "write %s", files[i].name);
    check(ok && written == files[i].size && close_err == DISK_OK, tag, &pass,
          &fail);
  }

  for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    char tag[80];
    snprintf(tag, sizeof(tag), "verify %s", files[i].name);
    check(verify_pattern_file(files[i].name, files[i].id, files[i].size,
                              files[i].chunk),
          tag, &pass, &fail);
  }

  for (uint32_t round = 0; round < rounds; round++) {
    uint32_t size = 2048 + (round % 9u) * 1536;
    uint32_t chunk = 1 + (round % 7u) * 73;
    uint32_t id = 2000u + round;
    uint32_t written = 0;
    disk_err_t close_err = DISK_OK;
    bool ok = write_pattern_file("ROUND.BIN", id, size, chunk, &written, &close_err);
    char tag[96];
    snprintf(tag, sizeof(tag), "round %lu overwrite write",
             (unsigned long)round + 1u);
    check(ok && written == size && close_err == DISK_OK, tag, &pass, &fail);

    snprintf(tag, sizeof(tag), "round %lu overwrite verify",
             (unsigned long)round + 1u);
    check(verify_pattern_file("ROUND.BIN", id, size, chunk + 31), tag, &pass, &fail);
  }

  err = f12_delete(&fs, "SMALL.BIN");
  check(err == DISK_OK, "delete SMALL.BIN", &pass, &fail);
  err = f12_delete(&fs, "MEDIUM.BIN");
  check(err == DISK_OK, "delete MEDIUM.BIN", &pass, &fail);
  check(f12_stat(&fs, "SMALL.BIN", &st) == DISK_ERR_NOT_FOUND, "SMALL.BIN gone",
        &pass, &fail);
  check(f12_stat(&fs, "MEDIUM.BIN", &st) == DISK_ERR_NOT_FOUND,
        "MEDIUM.BIN gone", &pass, &fail);

  uint32_t written = 0;
  disk_err_t close_err = DISK_OK;
  bool ok = write_pattern_file("REFILL.BIN", 3000, 12000, 333, &written, &close_err);
  check(ok && written == 12000 && close_err == DISK_OK, "write REFILL.BIN",
        &pass, &fail);
  check(verify_pattern_file("REFILL.BIN", 3000, 12000, 777), "verify REFILL.BIN",
        &pass, &fail);

  printf("\n--- Phase 6: Full Capacity and Fragmentation ---\n");
  uint16_t free_clusters = 0;
  err = f12_free_count(&fs, &free_clusters);
  bool capacity_ready = err == DISK_OK && free_clusters >= CAPACITY_FILE_COUNT;
  check(capacity_ready, "capacity workload has enough free clusters", &pass,
        &fail);
  if (!capacity_ready) goto done;

  memset(capacity_files, 0, sizeof(capacity_files));
  uint32_t random_state = 0x6D2B79F5u;
  uint32_t remaining_clusters = free_clusters;
  bool capacity_ok = true;
  for (uint32_t i = 0; i < CAPACITY_FILE_COUNT; i++) {
    uint32_t files_left = CAPACITY_FILE_COUNT - i;
    uint32_t clusters =
        capacity_partition(&random_state, remaining_clusters, files_left);
    if (!capacity_write(&capacity_files[i], 'R', i, clusters, &random_state)) {
      capacity_ok = false;
      break;
    }
    remaining_clusters -= clusters;
    if ((i + 1u) % 8u == 0u) {
      printf("  capacity: %lu/%u files, %lu clusters remain\n",
             (unsigned long)(i + 1u), CAPACITY_FILE_COUNT,
             (unsigned long)remaining_clusters);
    }
  }
  check(capacity_ok && remaining_clusters == 0u,
        "randomized files consume every free cluster", &pass, &fail);
  if (!capacity_ok || remaining_clusters != 0u) goto done;

  free_clusters = 1u;
  err = f12_free_count(&fs, &free_clusters);
  check(err == DISK_OK && free_clusters == 0u, "disk reports zero free space",
        &pass, &fail);
  if (err != DISK_OK || free_clusters != 0u) goto done;

  bool capacity_exact = capacity_verify();
  check(capacity_exact, "all randomized full-disk files are exact", &pass, &fail);
  if (!capacity_exact) goto done;

  disk_err_t overflow_open = writer_open("OVERFLOW.BIN");
  disk_result_t overflow_write = {.error = DISK_ERR_INVALID, .count = 0};
  if (overflow_open == DISK_OK) {
    work_buf[0] = gen_pattern_byte(0xFFFFFFFFu, 0);
    overflow_write = f12_write_full(&owned_writer, work_buf, 1);
  }
  disk_err_t overflow_abort = writer_abort();
  bool overflow_rejected =
      overflow_open == DISK_OK && overflow_write.error == DISK_ERR_FULL &&
      overflow_write.count == 0u && overflow_abort == DISK_OK;
  check(overflow_rejected, "new-file write reports disk full", &pass, &fail);
  if (!overflow_rejected) goto done;

  free_clusters = 1u;
  err = f12_free_count(&fs, &free_clusters);
  bool overflow_absent = f12_stat(&fs, "OVERFLOW.BIN", &st) == DISK_ERR_NOT_FOUND;
  check(err == DISK_OK && free_clusters == 0u && overflow_absent,
        "failed full-disk file remains unpublished", &pass, &fail);
  if (err != DISK_OK || free_clusters != 0u || !overflow_absent) goto done;

  const capacity_file_t *preserved = &capacity_files[0];
  char preserved_name[13];
  capacity_name(preserved, 0, preserved_name);
  disk_err_t replace_open = writer_open(preserved_name);
  disk_result_t replace_write = {.error = DISK_ERR_INVALID, .count = 0};
  if (replace_open == DISK_OK) {
    work_buf[0] = (uint8_t)(gen_pattern_byte(preserved->id, 0) ^ (uint8_t)0xFFu);
    replace_write = f12_write_full(&owned_writer, work_buf, 1);
  }
  disk_err_t replace_abort = writer_abort();
  bool replace_rejected = replace_open == DISK_OK &&
                          replace_write.error == DISK_ERR_FULL &&
                          replace_write.count == 0u && replace_abort == DISK_OK;
  check(replace_rejected, "copy-on-write write reports disk full", &pass, &fail);
  check(replace_rejected && verify_pattern_file(preserved_name, preserved->id,
                                                preserved->size, preserved->chunk),
        "failed replacement preserves original file", &pass, &fail);
  if (!replace_rejected) goto done;

  uint32_t freed_clusters = 0;
  uint32_t deleted_files = 0;
  bool delete_ok = true;
  for (uint32_t i = 1u; i < CAPACITY_FILE_COUNT; i += 3u) {
    char name[13];
    capacity_name(&capacity_files[i], i, name);
    if (f12_delete(&fs, name) != DISK_OK) {
      delete_ok = false;
      break;
    }
    freed_clusters += capacity_cluster_count(capacity_files[i].size);
    deleted_files++;
  }
  check(delete_ok, "delete randomized files across the full disk", &pass, &fail);
  if (!delete_ok) goto done;

  free_clusters = 0;
  err = f12_free_count(&fs, &free_clusters);
  check(err == DISK_OK && free_clusters == freed_clusters,
        "fragmented free-space count is exact", &pass, &fail);
  if (err != DISK_OK || free_clusters != freed_clusters) goto done;

  random_state = 0xB5297A4Du;
  remaining_clusters = freed_clusters;
  uint32_t refill_number = 0;
  bool refill_ok = true;
  for (uint32_t i = 1u; i < CAPACITY_FILE_COUNT; i += 3u) {
    uint32_t files_left = deleted_files - refill_number;
    uint32_t clusters =
        capacity_partition(&random_state, remaining_clusters, files_left);
    if (!capacity_write(&capacity_files[i], 'F', i, clusters, &random_state)) {
      refill_ok = false;
      break;
    }
    remaining_clusters -= clusters;
    refill_number++;
  }
  check(refill_ok && refill_number == deleted_files && remaining_clusters == 0u,
        "randomized refill consumes every fragmented cluster", &pass, &fail);
  if (!refill_ok || refill_number != deleted_files || remaining_clusters != 0u) {
    goto done;
  }

  free_clusters = 1u;
  err = f12_free_count(&fs, &free_clusters);
  check(err == DISK_OK && free_clusters == 0u,
        "fragmented refill returns disk to full", &pass, &fail);
  if (err != DISK_OK || free_clusters != 0u) goto done;

  printf("\n--- Phase 7: Remount Verify ---\n");
  err = unmount_filesystem();
  if (err == DISK_OK) err = do_mount();
  check_status(err, "remount after full-capacity stress", &pass, &fail);
  if (err != DISK_OK) goto done;
  check(verify_pattern_file("BYTEIO.BIN", 1000, 4096, 1),
        "BYTEIO.BIN survives remount", &pass, &fail);
  check(verify_pattern_file("LARGE.BIN", 1003, 32768, 4096),
        "LARGE.BIN survives remount", &pass, &fail);
  check(verify_pattern_file("REFILL.BIN", 3000, 12000, 777),
        "REFILL.BIN survives remount", &pass, &fail);
  check(capacity_verify(), "all fragmented files survive remount", &pass, &fail);
  free_clusters = 1u;
  err = f12_free_count(&fs, &free_clusters);
  check(err == DISK_OK && free_clusters == 0u, "full capacity survives remount",
        &pass, &fail);

  fat12_fsck_t report;
  err = f12_fsck(&fs, &report, false);
  check(err == DISK_OK && fat12_fsck_clean(&report),
        "filesystem is clean after remount", &pass, &fail);

  floppy_stats_t write_stats;
  disk_err_t stats_status = floppy_stats(&floppy, &write_stats);
  check(stats_status == DISK_OK, "query write-path statistics", &pass, &fail);
  if (stats_status == DISK_OK) {
    print_drive_stats(&write_stats);
    check(write_stats.dma_writes != 0, "verified DMA track writes observed",
          &pass, &fail);
    check(write_stats.underruns == 0 && write_stats.overruns == 0,
          "no write or read flow faults", &pass, &fail);
    check(write_stats.media_changes == 0, "no unexpected media changes", &pass,
          &fail);
  }
  current_generation = 0;
  generation_status = floppy_media_generation(&floppy, &current_generation);
  check(generation_status == DISK_OK && current_generation == test_generation,
        "media generation stable across writes and remount", &pass, &fail);

  printf("\n--- Phase 8: Full Disk Scan ---\n");
  diskdump_stats_t dump = run_diskdump(true);
  check(dump.total_valid == DISK_SECTOR_COUNT, "all sectors readable", &pass,
        &fail);
  check(dump.total_invalid == 0, "no unreadable sectors", &pass, &fail);
  check(dump.retries.failed == 0, "no final read failures", &pass, &fail);
  check(dump.retries.recovered <= 8, "recovered read retry threshold", &pass,
        &fail);
  check(dump.retries.overruns == 0, "no flux ring overruns", &pass, &fail);

done:
  if (fail != 0) {
    floppy_stats_t drive_stats;
    if (floppy_stats(&floppy, &drive_stats) == DISK_OK) print_drive_stats(&drive_stats);
  }
  cleanup = unmount_filesystem();
  if (cleanup != DISK_OK) {
    printf("Cleanup failed: %s\n", disk_strerror(cleanup));
    fail++;
  }
  if (!drive_idle()) fail++;

  printf("\n=== Test-Full Complete ===\n");
  printf("  Checks: %d passed, %d failed\n", pass, fail);
  printf("  Result: %s\n", fail == 0 ? "ALL PASSED" : "SOME FAILED");
}

static void cmd_test_media(int argc, char **argv) {
  (void)argc;
  (void)argv;

  disk_err_t error = writer_commit();
  if (error != DISK_OK) {
    printf("Cannot start test: %s\n", disk_strerror(error));
    return;
  }

  disk_err_t motor_status = floppy_motor_off(&floppy);
  disk_err_t select_status = floppy_select(&floppy, true);
  if (motor_status != DISK_OK || select_status != DISK_OK) {
    drive_status("Motor stop", motor_status);
    drive_status("Drive select", select_status);
    drive_idle();
    return;
  }

  f12_dir_t stale_dir;
  error = f12_opendir(&fs, "/", &stale_dir);
  if (error != DISK_OK) {
    printf("Cannot open stale-handle probe: %s\n", disk_strerror(error));
    drive_idle();
    return;
  }

  uint32_t original_generation = 0;
  disk_err_t generation_status =
      floppy_media_generation(&floppy, &original_generation);
  if (generation_status != DISK_OK) {
    drive_status("Media generation", generation_status);
    f12_closedir(&stale_dir);
    drive_idle();
    return;
  }

  printf("Eject the mounted disk, leave the drive empty, then enter y.\n");
  printf("Continue? [y/N] ");
  if (!cli_confirm()) {
    f12_closedir(&stale_dir);
    drive_idle();
    return;
  }

  int pass = 0;
  int fail = 0;
  bool changed = false;
  disk_err_t changed_status = floppy_disk_changed(&floppy, &changed);
  uint32_t replacement_generation = 0;
  generation_status = floppy_media_generation(&floppy, &replacement_generation);
  uint32_t expected_generation = original_generation + 1u;
  if (expected_generation == 0) expected_generation = 1u;
  bool generation_changed = generation_status == DISK_OK &&
                            replacement_generation == expected_generation;
  check(changed_status == DISK_OK && changed, "disk-change latch asserted",
        &pass, &fail);
  check(generation_changed, "media generation advanced exactly by event", &pass,
        &fail);

  disk_err_t stale_read = read_track(0, 0, original_generation);
  check(stale_read == DISK_ERR_MEDIA_CHANGED,
        "stale hardware generation is rejected", &pass, &fail);

  bool mounted = true;
  disk_err_t state_error = f12_is_mounted(&fs, &mounted);
  check(state_error == DISK_ERR_MEDIA_CHANGED && !mounted,
        "mounted filesystem observes media change", &pass, &fail);
  disk_err_t stale_close = f12_closedir(&stale_dir);
  check(stale_close == DISK_ERR_BAD_HANDLE,
        "stale filesystem handle is invalidated", &pass, &fail);

  if (!generation_changed || state_error != DISK_ERR_MEDIA_CHANGED) goto done;

  printf("Insert another canonical 1.44 MB FAT12 disk, then enter y.\n");
  printf("Continue? [y/N] ");
  if (!cli_confirm()) {
    drive_idle();
    return;
  }

  disk_err_t home_status = floppy_seek(&floppy, 0);
  check_status(home_status, "replacement disk homes successfully", &pass, &fail);
  changed = true;
  changed_status = floppy_disk_changed(&floppy, &changed);
  check(changed_status == DISK_OK && !changed,
        "disk-change latch clears after physical step", &pass, &fail);
  if (home_status != DISK_OK || changed_status != DISK_OK || changed) goto done;

  error = do_mount();
  check(error == DISK_OK, "replacement filesystem mounts", &pass, &fail);
  if (error == DISK_OK) {
    fat12_fsck_t report;
    error = f12_fsck(&fs, &report, false);
    check(error == DISK_OK && fat12_fsck_clean(&report),
          "replacement filesystem is clean", &pass, &fail);
  }

done:
  mounted = false;
  disk_err_t cleanup = f12_is_mounted(&fs, &mounted);
  if (cleanup == DISK_OK && mounted) cleanup = f12_unmount(&fs);
  if (cleanup != DISK_OK) {
    printf("Filesystem cleanup failed: %s\n", disk_strerror(cleanup));
    fail++;
  }
  if (!drive_idle()) fail++;

  printf("\n=== Test-Media Complete ===\n");
  printf("  Checks: %d passed, %d failed\n", pass, fail);
  printf("  Result: %s\n", fail == 0 ? "ALL PASSED" : "SOME FAILED");
}

static bool track_patch_fat(track_t *track, uint16_t fat_start, uint16_t cluster,
                            uint16_t value) {
  uint16_t first;
  if (!disk_chs_to_lba(track->cylinder, track->head, 0, &first)) return false;
  fat12_entry_loc_t loc = fat12_entry_locate(cluster);
  uint16_t lba = (uint16_t)(fat_start + loc.sector);
  uint16_t hi_lba = loc.split ? (uint16_t)(lba + 1u) : lba;
  if (lba < first || hi_lba >= first + DISK_SECTORS_PER_TRACK) return false;
  uint8_t *lo = &track->data[lba - first][loc.offset];
  uint8_t *hi = loc.split ? &track->data[hi_lba - first][0] : lo + 1;
  fat12_entry_pack(cluster, value, lo, hi);
  return true;
}

static bool patch_fat_track(bool both_copies, const fsck_scenario_patch_t *patches,
                            size_t count) {
  uint32_t generation;
  if (!drive_generation(&generation)) return false;
  if (!drive_status("FAT track read", read_track(0, 0, generation))) return false;
  for (size_t index = 0; index < count; index++) {
    bool patched = track_patch_fat(&work_track, FAT12_FAT2_START,
                                   patches[index].cluster, patches[index].value);
    if (both_copies) {
      patched = patched && track_patch_fat(&work_track, FAT12_FAT1_START,
                                           patches[index].cluster,
                                           patches[index].value);
    }
    if (!patched) {
      printf("FAT entry %u lies outside the FAT track.\n", patches[index].cluster);
      return false;
    }
  }
  return drive_status("FAT track write",
                      floppy_write_track(&floppy, generation, &work_track));
}

static bool fsck_scenario_files_exact(uint32_t truncated_size) {
  for (size_t index = 0; index < FSCK_SCENARIO_FILE_COUNT; index++) {
    const fsck_scenario_file_t *file = &fsck_scenario_files[index];
    uint32_t size = index == FSCK_SCENARIO_TRUNCATED_FILE ? truncated_size : file->size;
    if (!verify_pattern_file(file->name, 4000u + (uint32_t)index, size,
                             DISK_SECTOR_SIZE)) {
      return false;
    }
  }
  return true;
}

static void cmd_test_fsck(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("This will FORMAT the disk, damage its FAT, and run fsck repairs.\n");
  printf("Continue? [y/N] ");
  if (!cli_confirm()) return;

  disk_err_t start_error = unmount_filesystem();
  if (start_error != DISK_OK) {
    printf("Cannot start test: %s\n", disk_strerror(start_error));
    return;
  }

  int pass = 0;
  int fail = 0;
  disk_err_t cleanup;
  fat12_fsck_t report;

  printf("\n--- Phase 1: Format and Populate ---\n");
  disk_err_t err = do_format("FSCKTEST", false);
  check_status(err, "quick format", &pass, &fail);
  if (err != DISK_OK) goto done;
  err = do_mount();
  check_status(err, "mount", &pass, &fail);
  if (err != DISK_OK) goto done;
  for (size_t index = 0; index < FSCK_SCENARIO_FILE_COUNT; index++) {
    const fsck_scenario_file_t *file = &fsck_scenario_files[index];
    uint32_t written = 0;
    disk_err_t close_error = DISK_OK;
    bool ok = write_pattern_file(file->name, 4000u + (uint32_t)index, file->size,
                                 DISK_SECTOR_SIZE, &written, &close_error);
    char tag[64];
    snprintf(tag, sizeof(tag), "write %s", file->name);
    check(ok && written == file->size, tag, &pass, &fail);
    if (!ok) goto done;
  }
  err = f12_fsck(&fs, &report, false);
  check(err == DISK_OK && fat12_fsck_clean(&report), "fresh filesystem is clean",
        &pass, &fail);
  if (err != DISK_OK) goto done;
  err = unmount_filesystem();
  check(err == DISK_OK, "unmount before raw damage", &pass, &fail);
  if (err != DISK_OK) goto done;

  printf("\n--- Phase 2: Structural Damage ---\n");
  bool damaged = patch_fat_track(true, fsck_scenario_patches, FSCK_SCENARIO_PATCH_COUNT);
  check(damaged, "crosslink, loop, short chain and lost cluster written", &pass, &fail);
  if (!damaged) goto done;
  err = do_mount();
  check_status(err, "damaged filesystem mounts", &pass, &fail);
  if (err != DISK_OK) goto done;
  err = f12_fsck(&fs, &report, false);
  check(err == DISK_OK, "fsck check completes", &pass, &fail);
  if (err != DISK_OK) goto done;
  print_fsck_report(&report);
  check(fsck_scenario_damage_matches(&report), "damage report matches host suite",
        &pass, &fail);
  err = f12_fsck(&fs, &report, true);
  check(err == DISK_OK, "fsck repair completes", &pass, &fail);
  if (err != DISK_OK) goto done;
  print_fsck_report(&report);
  check(fsck_scenario_repair_matches(&report), "repair report matches host suite",
        &pass, &fail);
  err = f12_fsck(&fs, &report, false);
  check(err == DISK_OK && fat12_fsck_clean(&report), "repair converges", &pass, &fail);
  check(fsck_scenario_files_exact(FSCK_SCENARIO_TRUNCATED_SIZE),
        "surviving data is exact after repair", &pass, &fail);
  err = unmount_filesystem();
  check(err == DISK_OK, "unmount before mirror damage", &pass, &fail);
  if (err != DISK_OK) goto done;

  printf("\n--- Phase 3: FAT Mirror Damage ---\n");
  fsck_scenario_patch_t mirror = {FSCK_SCENARIO_MIRROR_CLUSTER,
                                  FSCK_SCENARIO_MIRROR_VALUE};
  damaged = patch_fat_track(false, &mirror, 1);
  check(damaged, "second FAT copy damaged", &pass, &fail);
  if (!damaged) goto done;
  err = do_mount();
  check_status(err, "mismatched filesystem mounts", &pass, &fail);
  if (err != DISK_OK) goto done;
  err = f12_fsck(&fs, &report, false);
  check(err == DISK_OK, "mirror check completes", &pass, &fail);
  if (err != DISK_OK) goto done;
  print_fsck_report(&report);
  check(fsck_scenario_mirror_matches(&report), "first FAT copy is authoritative",
        &pass, &fail);
  err = f12_fsck(&fs, &report, true);
  check(err == DISK_OK && report.repaired_fat2 && !report.repaired_fat1,
        "second FAT copy rewritten", &pass, &fail);
  err = f12_fsck(&fs, &report, false);
  check(err == DISK_OK && fat12_fsck_clean(&report), "mirror repair converges",
        &pass, &fail);
  check(fsck_scenario_files_exact(FSCK_SCENARIO_TRUNCATED_SIZE),
        "data survives mirror repair", &pass, &fail);

done:
  cleanup = unmount_filesystem();
  if (cleanup != DISK_OK) {
    printf("Cleanup failed: %s\n", disk_strerror(cleanup));
    fail++;
  }
  if (!drive_idle()) fail++;

  printf("\n=== Test-Fsck Complete ===\n");
  printf("  Checks: %d passed, %d failed\n", pass, fail);
  printf("  Result: %s\n", fail == 0 ? "ALL PASSED" : "SOME FAILED");
}

static void cmd_rpm(int argc, char **argv) {
  (void)argc;
  (void)argv;

  if (!drive_status("Drive select", floppy_select(&floppy, true))) return;
  if (!drive_status("Motor start", floppy_motor_on(&floppy))) {
    drive_status("Drive deselect", floppy_select(&floppy, false));
    return;
  }

  printf("  Measuring 5 index periods...\n");

  uint32_t periods[5];
  int got = 0;
  bool prev = gpio_get(drive_pins.index);
  absolute_time_t last = {0};
  bool have_last = false;
  absolute_time_t deadline = make_timeout_time_ms(3000);

  while (got < 5) {
    if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
      printf("  TIMEOUT: no index pulses. Disk inserted? Motor spinning?\n");
      goto cleanup;
    }
    bool now = gpio_get(drive_pins.index);
    if (now && !prev) {
      absolute_time_t t = get_absolute_time();
      if (have_last) periods[got++] = (uint32_t)absolute_time_diff_us(last, t);
      last = t;
      have_last = true;
    }
    prev = now;
  }

  uint32_t sum = 0;
  uint32_t min = ~0u;
  uint32_t max = 0;
  for (int i = 0; i < 5; i++) {
    sum += periods[i];
    if (periods[i] < min) min = periods[i];
    if (periods[i] > max) max = periods[i];
  }
  uint32_t avg = sum / 5;
  uint32_t rpm_x10 = avg ? 600000000u / avg : 0;
  int32_t dev_pm = avg ? (int32_t)(((int64_t)avg - 200000) * 1000 / 200000) : 0;
  int32_t dev_abs = dev_pm < 0 ? -dev_pm : dev_pm;

  printf("  Period:    avg %lu us (min %lu, max %lu, spread %lu)\n", avg, min,
         max, max - min);
  printf("  Speed:     %lu.%lu RPM (nominal 300.0)\n", rpm_x10 / 10, rpm_x10 % 10);
  printf("  Deviation: %s%ld.%ld%%\n", dev_pm < 0 ? "-" : "+", dev_abs / 10,
         dev_abs % 10);
  if (dev_abs > 50) {
    printf("  WARNING: more than 5%% off nominal -- writes may be unreliable\n");
  } else if (dev_abs > 15) {
    printf("  NOTE: more than 1.5%% off nominal, within decoder tolerance\n");
  } else {
    printf("  Spindle speed OK\n");
  }

cleanup:
  drive_idle();
}

static void cmd_crashtest(int argc, char **argv) {
  (void)argc;
  (void)argv;

  printf("This will format the disk and overwrite TARGET.BIN until power is "
         "cut.\n");
  printf("Continue? [y/N] ");
  if (!cli_confirm()) return;

  disk_err_t unmount_error = unmount_filesystem();
  if (unmount_error != DISK_OK) {
    printf("Cannot start test: %s\n", disk_strerror(unmount_error));
    return;
  }

  disk_err_t error = do_format("CRASH", false);
  if (error != DISK_OK) {
    printf("Format failed: %s\n", disk_strerror(error));
    return;
  }
  error = do_mount();
  if (error != DISK_OK) {
    printf("Mount failed: %s\n", disk_strerror(error));
    return;
  }

  crash_state_t state;
  memset(&state, 0, sizeof(state));
  memcpy(state.magic, CRASH_STATE_MAGIC, sizeof(state.magic));
  state.version = CRASH_STATE_VERSION;
  state.stable_id = 9000;
  state.stable_size = 8u * DISK_SECTOR_SIZE;
  state.target_old_id = 9200;
  state.target_new_id = 9201;
  state.target_size = 32u * DISK_SECTOR_SIZE;
  state.filler_id = 9100;

  uint32_t written = 0;
  disk_err_t close_error = DISK_OK;
  if (!write_pattern_file(CRASH_STABLE_FILE, state.stable_id, state.stable_size,
                          DISK_SECTOR_SIZE, &written, &close_error) ||
      written != state.stable_size) {
    printf("Stable-file write failed: %s\n", disk_strerror(close_error));
    goto cleanup;
  }
  if (!write_pattern_file(CRASH_TARGET_FILE, state.target_old_id, state.target_size,
                          DISK_SECTOR_SIZE, &written, &close_error) ||
      written != state.target_size) {
    printf("Baseline target write failed: %s\n", disk_strerror(close_error));
    goto cleanup;
  }

  uint16_t available = 0;
  error = f12_free_count(&fs, &available);
  uint32_t target_clusters = (state.target_size + CLUSTER_SIZE - 1u) / CLUSTER_SIZE;
  if (error != DISK_OK) {
    printf("Free-space query failed: %s\n", disk_strerror(error));
    goto cleanup;
  }
  if (available <= target_clusters + 1u) {
    printf("Disk has %u free clusters; the test requires more than %lu.\n",
           available, (unsigned long)(target_clusters + 1u));
    goto cleanup;
  }
  state.filler_size = ((uint32_t)available - target_clusters - 1u) * CLUSTER_SIZE;
  if (!crash_state_store(&state)) {
    printf("Crash-state write failed.\n");
    goto cleanup;
  }
  if (!write_pattern_file(CRASH_FILLER_FILE, state.filler_id, state.filler_size,
                          8u * DISK_SECTOR_SIZE, &written, &close_error) ||
      written != state.filler_size) {
    printf("Filler write failed: %s\n", disk_strerror(close_error));
    goto cleanup;
  }

  printf("Prepared stable=%lu, target=%lu, filler=%lu bytes.\n",
         (unsigned long)state.stable_size, (unsigned long)state.target_size,
         (unsigned long)state.filler_size);
  printf("Pull power during an overwrite. Press any key after a completed "
         "round to stop.\n");

  for (uint32_t round = 1;; round++) {
    uint32_t pattern = (round & 1u) ? state.target_new_id : state.target_old_id;
    bool ok = write_pattern_file(CRASH_TARGET_FILE, pattern, state.target_size,
                                 DISK_SECTOR_SIZE, &written, &close_error);
    if (!ok || written != state.target_size) {
      printf("Round %lu failed: %s\n", (unsigned long)round, disk_strerror(close_error));
      goto cleanup;
    }
    printf("Round %lu committed.\n", (unsigned long)round);
    if (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {
      printf("Stopped after a complete commit.\n");
      goto cleanup;
    }
  }

cleanup:
  error = unmount_filesystem();
  if (error != DISK_OK) printf("Filesystem cleanup failed: %s\n", disk_strerror(error));
  drive_idle();
}

static void cmd_crashcheck(int argc, char **argv) {
  (void)argc;
  (void)argv;

  disk_err_t unmount_error = unmount_filesystem();
  if (unmount_error != DISK_OK) {
    printf("Cannot remount: %s\n", disk_strerror(unmount_error));
    return;
  }

  int pass = 0;
  int fail = 0;
  disk_err_t cleanup_error;
  disk_err_t error = do_mount();
  check_status(error, "disk mounts after power cut", &pass, &fail);
  if (error != DISK_OK) {
    printf("Run 'crashtest' first.\n");
    goto cleanup;
  }

  crash_state_t state;
  bool state_valid = crash_state_load(&state);
  check(state_valid, "crash state is exact and checksummed", &pass, &fail);
  if (!state_valid) goto cleanup;

  fat12_fsck_t report;
  error = f12_fsck(&fs, &report, false);
  check(error == DISK_OK, "fsck check completes", &pass, &fail);
  if (error != DISK_OK) goto cleanup;
  print_fsck_report(&report);

  error = f12_fsck(&fs, &report, true);
  check(error == DISK_OK, "fsck repair completes", &pass, &fail);
  if (error != DISK_OK) goto cleanup;
  error = f12_fsck(&fs, &report, false);
  check(error == DISK_OK && fat12_fsck_clean(&report), "fsck repair converges",
        &pass, &fail);
  if (error != DISK_OK || !fat12_fsck_clean(&report)) goto cleanup;

  check(verify_pattern_file(CRASH_STABLE_FILE, state.stable_id, state.stable_size,
                            DISK_SECTOR_SIZE),
        "stable file is exact", &pass, &fail);
  check(verify_pattern_file(CRASH_FILLER_FILE, state.filler_id, state.filler_size,
                            8u * DISK_SECTOR_SIZE),
        "filler file is exact", &pass, &fail);

  bool old_exact = verify_pattern_file(CRASH_TARGET_FILE, state.target_old_id,
                                       state.target_size, DISK_SECTOR_SIZE);
  bool new_exact = verify_pattern_file(CRASH_TARGET_FILE, state.target_new_id,
                                       state.target_size, DISK_SECTOR_SIZE);
  check(old_exact != new_exact, "target is exactly one recorded generation",
        &pass, &fail);

cleanup:
  cleanup_error = unmount_filesystem();
  if (cleanup_error != DISK_OK) {
    printf("Filesystem cleanup failed: %s\n", disk_strerror(cleanup_error));
    fail++;
  }
  if (!drive_idle()) fail++;

  printf("\n=== Crashcheck Complete ===\n");
  printf("  Checks: %d passed, %d failed\n", pass, fail);
  printf("  Result: %s\n", fail == 0 ? "ALL PASSED" : "SOME FAILED");
}

static diskdump_stats_t run_diskdump(bool verbose) {
  diskdump_stats_t stats;
  memset(&stats, 0, sizeof(stats));

  uint32_t disk_checksum = 5381u;
  uint32_t generation;
  if (!drive_generation(&generation)) return stats;
  if (!drive_status("Statistics reset", floppy_stats_reset(&floppy))) return stats;

  if (verbose) {
    printf("  %-8s %-6s %-10s %-10s\n", "TRACK", "SIDE", "DECODED", "STATUS");
    printf("  %-8s %-6s %-10s %-10s\n", "-----", "----", "-------", "------");
  }

  for (uint8_t track = 0; track < DISK_CYLINDERS; track++) {
    for (uint8_t side = 0; side < DISK_HEADS; side++) {
      disk_err_t status = read_track(track, side, generation);
      unsigned decoded = track_sectors(&work_track);
      unsigned errors = DISK_SECTORS_PER_TRACK - decoded;
      for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
        if (track_has(&work_track, sector)) {
          disk_checksum = checksum_extend(disk_checksum, work_track.data[sector],
                                          DISK_SECTOR_SIZE);
        }
      }
      stats.total_valid += (int)decoded;
      stats.total_invalid += (int)errors;
      if (verbose || status != DISK_OK) {
        printf("  T%02u      %u      %2u/%-2u      %s\n", track, side, decoded,
               DISK_SECTORS_PER_TRACK, disk_strerror(status));
      }
    }
  }

  stats.checksum = disk_checksum;
  if (!drive_status("Statistics query", floppy_stats(&floppy, &stats.retries))) {
    stats.total_invalid++;
  }

  printf("\n  Total decoded: %d / %u\n", stats.total_valid, DISK_SECTOR_COUNT);
  printf("  Errors:        %d\n", stats.total_invalid);
  printf("  Disk checksum: 0x%08lX\n", disk_checksum);
  print_drive_stats(&stats.retries);
  return stats;
}

static void cmd_diskdump(int argc, char **argv) {
  bool verbose = true;
  if (argc == 2) {
    if (!text_equal_case(argv[1], "quiet")) {
      printf("Disk-dump mode must be 'quiet'.\n");
      return;
    }
    verbose = false;
  }
  run_diskdump(verbose);
}

static void cmd_mfmscan(int argc, char **argv) {
  (void)argc;
  (void)argv;
  unsigned clean = 0;
  unsigned failed = 0;
  if (!drive_status("Statistics reset", floppy_stats_reset(&floppy))) return;
  printf("  C/H   STATUS       REVS MIN SHORT MEDIUM LONG INVALID CRC FORMAT\n");
  for (uint8_t cylinder = 0; cylinder < DISK_CYLINDERS; cylinder++) {
    for (uint8_t head = 0; head < DISK_HEADS; head++) {
      disk_err_t error = read_track_stats(cylinder, head, &signal_probe);
      mfm_probe_counts_t counts = mfm_probe_counts(&signal_probe);
      printf("  %02u/%u  %-12s %lu %lu %lu %lu %lu %lu %lu %lu\n",
             cylinder, head, disk_strerror(error),
             (unsigned long)signal_probe.revolutions,
             (unsigned long)signal_probe.minimum_sectors,
             (unsigned long)counts.short_count, (unsigned long)counts.medium_count,
             (unsigned long)counts.long_count, (unsigned long)counts.invalid_count,
             (unsigned long)signal_probe.decoder.crc_errors,
             (unsigned long)signal_probe.decoder.format_errors);
      if (error == DISK_OK && mfm_probe_clean(&signal_probe)) clean++;
      else failed++;
      if (error != DISK_OK) goto done;
    }
  }
done:
  printf("  Both heads: %u clean tracks, %u failed, %u untested\n", clean, failed,
         DISK_TRACK_COUNT - clean - failed);
  floppy_stats_t stats;
  if (drive_status("Statistics", floppy_stats(&floppy, &stats))) {
    print_drive_stats(&stats);
  }
  drive_idle();
}

static void cmd_version(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("Pico Floppy %s\n", FW_VERSION);
  printf("  board:     %s\n", BOARD_NAME);
  printf("  sys clock: %lu MHz\n", (unsigned long)(clock_get_hz(clk_sys) / 1000000));
}

static void cmd_reboot(int argc, char **argv) {
  (void)argc;
  (void)argv;
  disk_err_t error = unmount_filesystem();
  if (error != DISK_OK) {
    printf("Reboot refused: %s\n", disk_strerror(error));
    return;
  }
  error = floppy_deinit(&floppy);
  if (error != DISK_OK) {
    printf("Reboot refused: %s\n", disk_strerror(error));
    return;
  }
  printf("Rebooting...\n");
  sleep_ms(100);
  watchdog_reboot(0, 0, 0);
  for (;;) tight_loop_contents();
}

int main(void) {
  bool clock_ready = board_clock_init();
  stdio_init_all();
  sleep_ms(2000);

  printf("\r\n\r\n=== Pico Floppy Shell ===\r\n");
  print_reset_cause();
  if (!clock_ready) {
    printf("Clock initialization failed: requested %lu kHz, running %lu kHz\r\n",
           (unsigned long)BOARD_SYS_CLOCK_KHZ,
           (unsigned long)(clock_get_hz(clk_sys) / 1000u));
    for (;;) tight_loop_contents();
  }
  cmd_version(0, NULL);

  disk_err_t error = floppy_init(&floppy, drive_pins);
  if (error != DISK_OK) {
    printf("Drive initialization failed: %s\r\n", disk_strerror(error));
    for (;;) tight_loop_contents();
  }

  error = f12_init(&fs, floppy_device(&floppy));
  if (error != DISK_OK) {
    printf("Filesystem initialization failed: %s\r\n", disk_strerror(error));
    disk_err_t cleanup_error = floppy_deinit(&floppy);
    if (cleanup_error != DISK_OK) {
      printf("Drive cleanup failed: %s\r\n", disk_strerror(cleanup_error));
    }
    for (;;) tight_loop_contents();
  }

  printf("Drive initialized (HD mode)\r\n");
  printf("Type 'help' for commands, 'mount' when disk is ready.\r\n\r\n");

  char *argv[MAX_ARGS];
  for (;;) {
    print_prompt();
    int len = cli_readline(cmd_buf);
    if (len == CLI_INPUT_CANCELLED) continue;
    if (len == CLI_INPUT_OVERFLOW) {
      printf("Command line too long.\n");
      continue;
    }
    if (len == 0) continue;

    int argc = tokenize(cmd_buf, argv, MAX_ARGS);
    if (argc < 0) {
      printf("Too many arguments.\n");
      continue;
    }
    if (argc == 0) continue;

    const cmd_entry_t *cmd = find_command(argv[0]);
    if (!cmd) {
      printf("Unknown command '%s'. Type 'help' for commands.\n", argv[0]);
      continue;
    }
    if (argc < cmd->min_args || argc > cmd->max_args) {
      print_usage(cmd);
      continue;
    }
    if (cmd->needs_mount) {
      bool mounted;
      error = f12_is_mounted(&fs, &mounted);
      if (error != DISK_OK) {
        printf("Filesystem state check failed: %s\n", disk_strerror(error));
        continue;
      }
      if (!mounted) {
        printf("Not mounted. Use 'mount' first.\n");
        continue;
      }
    }
    cmd->fn(argc, argv);
  }
  return 0;
}
