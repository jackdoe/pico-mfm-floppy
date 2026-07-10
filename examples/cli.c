#include "f12.h"
#include "floppy.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "mfm_decode.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define FW_VERSION "0.2.0"
#define CMD_BUF_SIZE 256
#define IO_BUF_SIZE DISK_SECTOR_SIZE
#define WORK_BUF_SIZE (8u * DISK_SECTOR_SIZE)
#define MAX_ARGS 4
#define PULSE_BINS 128
#define WRITER_CLOSE_ATTEMPTS 3
#define CLUSTER_SIZE (FAT12_MAX_CLUSTER_SECTORS * DISK_SECTOR_SIZE)
#define CAPACITY_FILE_COUNT 64u
#define CAPACITY_MAX_FILE_CLUSTERS 96u
#define CLI_INPUT_OVERFLOW (-1)
#define CLI_INPUT_CANCELLED (-2)

static floppy_t floppy;
static f12_t fs;
static f12_file_t owned_writer;
static bool writer_owned;
static const floppy_pins_t drive_pins = {
#ifdef FLOPPY_ALT_PINS
    .index = 16,
    .track0 = 22,
    .write_protect = 13,
    .read_data = 26,
    .disk_change = 28,
    .drive_select = 14,
    .motor_enable = 17,
    .direction = 18,
    .step = 19,
    .write_data = 20,
    .write_gate = 21,
    .side_select = 27,
    .density = 15,
#else
    .index = 14,
    .track0 = 5,
    .write_protect = 4,
    .read_data = 3,
    .disk_change = 1,
    .drive_select = 12,
    .motor_enable = 10,
    .direction = 9,
    .step = 8,
    .write_data = 7,
    .write_gate = 6,
    .side_select = 2,
    .density = 15,
#endif
};

static char cmd_buf[CMD_BUF_SIZE];
static uint8_t io_buf[IO_BUF_SIZE];
static uint8_t work_buf[WORK_BUF_SIZE];

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
static void cmd_crashtest(int argc, char **argv);
static void cmd_crashcheck(int argc, char **argv);
static void cmd_rpm(int argc, char **argv);
static void cmd_test_full(int argc, char **argv);
static void cmd_test_media(int argc, char **argv);
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
    {"rpm", cmd_rpm, false, 1, 1, "", "Measure spindle speed"},
    {"test-full", cmd_test_full, false, 1, 2, "[rounds]",
     "Run destructive hardware tests"},
    {"test-media", cmd_test_media, true, 1, 1, "",
     "Run interactive media-change tests"},
    {"crashtest", cmd_crashtest, false, 1, 1, "",
     "Prepare and loop for power-cut testing"},
    {"crashcheck", cmd_crashcheck, false, 1, 1, "", "Verify after a power cut"},
    {"diskdump", cmd_diskdump, false, 1, 2, "[quiet]", "Scan every sector"},
    {"mfmscan", cmd_mfmscan, false, 1, 1, "", "Scan MFM quality"},
    {"reboot", cmd_reboot, false, 1, 1, "", "Safely reboot"},
    {"version", cmd_version, false, 1, 1, "", "Show firmware version"},
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

static f12_err_t do_mount(void) { return f12_mount(&fs); }

static bool writer_handle_gone(f12_err_t error) {
  return error == F12_ERR_BAD_HANDLE || error == F12_ERR_MEDIA_CHANGED ||
         error == F12_ERR_NOT_MOUNTED || error == F12_ERR_NOT_INITIALIZED;
}

static void writer_forget(void) {
  memset(&owned_writer, 0, sizeof(owned_writer));
  writer_owned = false;
}

static f12_err_t writer_commit(void) {
  if (!writer_owned)
    return F12_OK;
  f12_err_t error = F12_OK;
  for (unsigned attempt = 0; attempt < WRITER_CLOSE_ATTEMPTS; attempt++) {
    error = f12_close(&owned_writer);
    if (error == F12_OK) {
      writer_forget();
      return F12_OK;
    }
    if (writer_handle_gone(error)) {
      writer_forget();
      return error;
    }
  }
  return error;
}

static f12_err_t writer_abort(void) {
  if (!writer_owned)
    return F12_OK;
  f12_err_t error = f12_abort(&owned_writer);
  if (error == F12_OK || writer_handle_gone(error))
    writer_forget();
  return error;
}

static f12_err_t writer_open(const char *name) {
  f12_err_t error = writer_commit();
  if (error != F12_OK)
    return error;
  error = f12_open(&fs, name, F12_OPEN_WRITE, &owned_writer);
  writer_owned = error == F12_OK;
  return error;
}

static f12_err_t unmount_filesystem(void) {
  f12_err_t error = writer_commit();
  if (error != F12_OK)
    return error;
  bool mounted;
  error = f12_is_mounted(&fs, &mounted);
  if (error != F12_OK)
    return error;
  return mounted ? f12_unmount(&fs) : F12_OK;
}

static f12_result_t f12_write_full(f12_file_t *file, const void *buf,
                                   uint32_t len) {
  f12_result_t total = {.error = F12_OK, .count = 0};
  while (total.count < len) {
    uint32_t chunk = len - (uint32_t)total.count;
    if (chunk > DISK_SECTOR_SIZE)
      chunk = DISK_SECTOR_SIZE;
    f12_result_t result =
        f12_write(file, (const uint8_t *)buf + total.count, chunk);
    if (result.count > chunk) {
      total.error = F12_ERR_IO;
      return total;
    }
    total.count += result.count;
    if (result.error != F12_OK) {
      total.error = result.error;
      return total;
    }
    if (result.count == 0) {
      total.error = F12_ERR_IO;
      return total;
    }
  }
  return total;
}

static f12_result_t f12_read_full(f12_file_t *file, void *buf,
                                  uint32_t capacity) {
  f12_result_t total = {.error = F12_OK, .count = 0};
  while (total.count < capacity) {
    size_t remaining = (size_t)capacity - total.count;
    size_t chunk = remaining < DISK_SECTOR_SIZE ? remaining : DISK_SECTOR_SIZE;
    f12_result_t result = f12_read(file, (uint8_t *)buf + total.count, chunk);
    if (result.count > chunk) {
      total.error = F12_ERR_IO;
      return total;
    }
    total.count += result.count;
    if (result.error == F12_END)
      return total;
    if (result.error != F12_OK) {
      total.error = result.error;
      return total;
    }
    if (result.count == 0) {
      total.error = F12_ERR_IO;
      return total;
    }
  }
  return total;
}

static bool text_equal_case(const char *left, const char *right);

static bool canonical_name(const char *input, char output[13]) {
  fat12_name_t name;
  if (fat12_name_parse(input, &name) != FAT12_OK)
    return false;
  size_t pos = 0;
  for (size_t i = 0; i < sizeof(name.name) && name.name[i] != ' '; i++) {
    output[pos++] = name.name[i];
  }
  size_t ext = 0;
  while (ext < sizeof(name.ext) && name.ext[ext] != ' ')
    ext++;
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
  f12_err_t error = f12_is_mounted(&fs, &mounted);
  if (error != F12_OK)
    printf("[??]> ");
  else if (mounted)
    printf("[A:]> ");
  else
    printf("[--]> ");
}

static int cli_readline(char buf[static CMD_BUF_SIZE]) {
  size_t pos = 0;
  bool overflow = false;
  memset(buf, 0, CMD_BUF_SIZE);

  for (;;) {
    int c = getchar();
    if (c == EOF) {
      tight_loop_contents();
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
      if (overflow) {
        continue;
      } else if (pos > 0) {
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
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '\0')
      break;
    if (argc == max_args)
      return -1;
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
    if (*p)
      *p++ = '\0';
  }
  return argc;
}

static bool parse_u32(const char *text, uint32_t minimum, uint32_t maximum,
                      uint32_t *value) {
  if (!text || !*text || !value || minimum > maximum)
    return false;
  uint32_t parsed = 0;
  for (const unsigned char *cursor = (const unsigned char *)text;
       *cursor != '\0'; cursor++) {
    if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9')
      return false;
    uint32_t digit = (uint32_t)(*cursor - (unsigned char)'0');
    if (parsed > maximum / 10u ||
        (parsed == maximum / 10u && digit > maximum % 10u)) {
      return false;
    }
    parsed = parsed * 10u + digit;
  }
  if (parsed < minimum)
    return false;
  *value = parsed;
  return true;
}

static unsigned char ascii_fold(unsigned char value) {
  if (value >= (unsigned char)'A' && value <= (unsigned char)'Z') {
    return (unsigned char)(value + ((unsigned char)'a' - (unsigned char)'A'));
  }
  return value;
}

static bool text_equal_case(const char *left, const char *right) {
  if (!left || !right)
    return false;
  while (*left != '\0' && *right != '\0') {
    if (ascii_fold((unsigned char)*left) != ascii_fold((unsigned char)*right)) {
      return false;
    }
    left++;
    right++;
  }
  return *left == *right;
}

static const cmd_entry_t *find_command(const char *name) {
  for (unsigned i = 0; i < NUM_COMMANDS; i++) {
    if (text_equal_case(name, commands[i].name))
      return &commands[i];
  }
  return NULL;
}

static void print_usage(const cmd_entry_t *command) {
  printf("Usage: %s%s%s\n", command->name,
         command->syntax[0] == '\0' ? "" : " ", command->syntax);
}

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
  bool seen[DISK_SECTORS_PER_TRACK];
} track_stats_t;

typedef struct {
  int total_valid;
  int total_invalid;
  uint32_t checksum;
  floppy_stats_t retries;
} diskdump_stats_t;

static diskdump_stats_t run_diskdump(bool verbose);

static const char *block_status_text(block_status_t status) {
  switch (status) {
  case BLOCK_OK:
    return "success";
  case BLOCK_ERR_INVALID:
    return "invalid state or argument";
  case BLOCK_ERR_BUSY:
    return "busy";
  case BLOCK_ERR_TIMEOUT:
    return "timeout";
  case BLOCK_ERR_CRC:
    return "CRC error";
  case BLOCK_ERR_WRONG_TRACK:
    return "wrong track";
  case BLOCK_ERR_WRONG_SIDE:
    return "wrong side";
  case BLOCK_ERR_NO_TRACK0:
    return "track zero unavailable";
  case BLOCK_ERR_MEDIA_CHANGED:
    return "media changed";
  case BLOCK_ERR_WRITE_PROTECTED:
    return "write protected";
  case BLOCK_ERR_UNDERRUN:
    return "underrun";
  case BLOCK_ERR_OVERRUN:
    return "overrun";
  case BLOCK_ERR_VERIFY:
    return "verification failed";
  case BLOCK_ERR_CORRUPT:
    return "corrupt data";
  case BLOCK_ERR_IO:
    return "I/O error";
  }
  return "unknown error";
}

static bool drive_status(const char *operation, block_status_t status) {
  if (status == BLOCK_OK)
    return true;
  printf("%s failed: %s\n", operation, block_status_text(status));
  return false;
}

static bool drive_idle(void) {
  bool idle = drive_status("Motor stop", floppy_motor_off(&floppy));
  if (!drive_status("Drive deselect", floppy_select(&floppy, false)))
    idle = false;
  return idle;
}

static bool drive_generation(uint32_t *generation) {
  return drive_status("Media generation",
                      floppy_media_generation(&floppy, generation));
}

static void print_drive_stats(const floppy_stats_t *s) {
  printf("  Sector reads:  %lu\n", s->reads);
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
  if (!drive_status("Statistics reset", floppy_stats_reset(&floppy)))
    return;
  uint32_t generation;
  if (!drive_generation(&generation))
    return;
  track_t track = {.cylinder = (uint8_t)cylinder, .head = (uint8_t)head};
  block_status_t status = floppy_read_track(&floppy, generation, &track);
  printf("  Track C%luH%lu read: %s\n", (unsigned long)cylinder,
         (unsigned long)head, block_status_text(status));
  if (status == BLOCK_OK) {
    unsigned recovered = 0;
    for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++)
      if (track_has(&track, sector))
        recovered++;
    printf("  Sectors:       %u/%u recovered\n", recovered,
           DISK_SECTORS_PER_TRACK);
  }
  floppy_stats_t stats;
  if (drive_status("Statistics", floppy_stats(&floppy, &stats)))
    print_drive_stats(&stats);
  drive_idle();
}

static void read_track_stats(int track, int side, track_stats_t *stats) {
  memset(stats, 0, sizeof(*stats));

  if (!drive_status("Seek", floppy_seek(&floppy, (uint8_t)track)))
    return;
  if (!drive_status("Side select", floppy_side_select(&floppy, (uint8_t)side)))
    return;
  uint32_t generation;
  if (!drive_generation(&generation))
    return;
  if (!drive_status("Flux capture", floppy_flux_begin(&floppy, generation)))
    return;

  mfm_t mfm;
  mfm_init(&mfm);

  bool ix_prev = false;
  bool ix_primed = false;
  mfm_sector_t sector;
  int ix_edges = 0;

  while (ix_edges < 6) {
    uint16_t delta;
    bool ix;
    block_status_t status = floppy_flux_next(&floppy, &delta, &ix);
    if (status != BLOCK_OK) {
      drive_status("Flux read", status);
      break;
    }

    if (ix_primed && ix != ix_prev)
      ix_edges++;
    ix_prev = ix;
    ix_primed = true;

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

    if (mfm_feed(&mfm, delta, &sector) &&
        sector.sector < DISK_SECTORS_PER_TRACK) {
      uint8_t idx = sector.sector;
      if (!stats->seen[idx]) {
        stats->seen[idx] = true;
        stats->unique_sectors++;
      }
    }
  }

  drive_status("Flux stop", floppy_flux_end(&floppy));

  stats->T2_max = mfm.T2_max;
  stats->T3_max = mfm.T3_max;
  stats->syncs = mfm.syncs_found;
  stats->sector_records = mfm.sectors_read;
  stats->crc_errors = mfm.crc_errors;
}

static void print_histogram(track_stats_t *stats) {
  uint32_t peak = 0;
  for (int i = 0; i < PULSE_BINS; i++) {
    if (stats->histogram[i] > peak)
      peak = stats->histogram[i];
  }
  if (peak == 0)
    return;

  int first = 0, last = PULSE_BINS - 1;
  while (first < PULSE_BINS && stats->histogram[first] == 0)
    first++;
  while (last > first && stats->histogram[last] == 0)
    last--;

  printf("  Pulse Distribution (delta ticks):\n");
  for (int i = first; i <= last; i++) {
    if (stats->histogram[i] == 0)
      continue;
    uint32_t bar = (stats->histogram[i] * 50u) / peak;
    printf("  %3d: %6lu |", i, stats->histogram[i]);
    for (uint32_t j = 0; j < bar; j++)
      printf("#");
    printf("\n");
  }
}

static uint8_t gen_pattern_byte(uint32_t file_id, uint32_t offset) {
  uint32_t v = file_id * 2654435761u + offset * 40503u;
  return (uint8_t)((v >> 16) & 0xFFu);
}

static uint32_t checksum_extend(uint32_t sum, const uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    sum = (sum << 5) + sum + buf[i];
  }
  return sum;
}

static void fill_pattern_range(uint8_t *buf, uint32_t file_id, uint32_t offset,
                               uint32_t size) {
  for (uint32_t i = 0; i < size; i++) {
    buf[i] = gen_pattern_byte(file_id, offset + i);
  }
}

static bool write_pattern_file_chunked(const char *name, uint32_t file_id,
                                       uint32_t size, uint32_t chunk_size,
                                       uint32_t *written_out,
                                       f12_err_t *close_err_out) {
  if (chunk_size == 0 || chunk_size > WORK_BUF_SIZE) {
    chunk_size = WORK_BUF_SIZE;
  }

  f12_err_t open_error = writer_open(name);
  if (open_error != F12_OK) {
    if (written_out)
      *written_out = 0;
    if (close_err_out)
      *close_err_out = open_error;
    return false;
  }

  uint32_t written = 0;
  while (written < size) {
    uint32_t chunk = size - written;
    if (chunk > chunk_size)
      chunk = chunk_size;
    fill_pattern_range(work_buf, file_id, written, chunk);

    f12_result_t result = f12_write_full(&owned_writer, work_buf, chunk);
    written += (uint32_t)result.count;
    if (result.error != F12_OK || result.count != chunk) {
      f12_err_t abort_error = writer_abort();
      if (written_out)
        *written_out = written;
      if (close_err_out) {
        *close_err_out =
            abort_error == F12_OK
                ? (result.error == F12_OK ? F12_ERR_IO : result.error)
                : abort_error;
      }
      return false;
    }
  }

  if (written_out)
    *written_out = written;
  f12_err_t close_error = writer_commit();
  if (close_err_out)
    *close_err_out = close_error;
  return close_error == F12_OK;
}

static bool verify_pattern_file_chunked(const char *name, uint32_t file_id,
                                        uint32_t size, uint32_t chunk_size,
                                        uint32_t *read_out) {
  if (chunk_size == 0 || chunk_size > WORK_BUF_SIZE) {
    chunk_size = WORK_BUF_SIZE;
  }

  f12_stat_t stat;
  if (f12_stat(&fs, name, &stat) != F12_OK || stat.size != size) {
    if (read_out)
      *read_out = 0;
    return false;
  }
  f12_file_t file;
  if (f12_open(&fs, name, F12_OPEN_READ, &file) != F12_OK) {
    if (read_out)
      *read_out = 0;
    return false;
  }

  bool ok = true;
  uint32_t total = 0;
  while (total < size) {
    uint32_t chunk = size - total;
    if (chunk > chunk_size)
      chunk = chunk_size;

    f12_result_t result = f12_read(&file, work_buf, chunk);
    if (result.count > chunk || result.error != F12_OK || result.count == 0) {
      ok = false;
      break;
    }

    for (size_t i = 0; i < result.count; i++) {
      if (work_buf[i] != gen_pattern_byte(file_id, total + (uint32_t)i)) {
        ok = false;
        break;
      }
    }
    if (!ok)
      break;

    total += (uint32_t)result.count;
  }

  if (read_out)
    *read_out = total;
  if (f12_close(&file) != F12_OK)
    ok = false;
  return ok && total == size;
}

static uint32_t capacity_random(uint32_t *state) {
  *state = *state * 1664525u + 1013904223u;
  return *state;
}

static uint32_t capacity_partition(uint32_t *state, uint32_t clusters,
                                   uint32_t files) {
  if (files == 1u)
    return clusters;
  uint32_t available = clusters - (files - 1u);
  uint32_t limit = available;
  if (limit > CAPACITY_MAX_FILE_CLUSTERS)
    limit = CAPACITY_MAX_FILE_CLUSTERS;
  return 1u + capacity_random(state) % limit;
}

static uint32_t capacity_cluster_count(uint32_t size) {
  return (size + CLUSTER_SIZE - 1u) / CLUSTER_SIZE;
}

static void capacity_name(const capacity_file_t *file, uint32_t number,
                          char name[13]) {
  snprintf(name, 13, "%c%07lu.BIN", file->prefix, (unsigned long)number);
}

static void capacity_prepare(capacity_file_t *file, char prefix,
                             uint32_t clusters, uint32_t *state) {
  file->prefix = prefix;
  file->id = capacity_random(state);
  file->size = (clusters - 1u) * CLUSTER_SIZE + 1u +
               capacity_random(state) % CLUSTER_SIZE;
  file->chunk = 1u + capacity_random(state) % WORK_BUF_SIZE;
}

static bool capacity_write(capacity_file_t *file, char prefix, uint32_t number,
                           uint32_t clusters, uint32_t *state) {
  capacity_prepare(file, prefix, clusters, state);
  char name[13];
  capacity_name(file, number, name);
  uint32_t written = 0;
  f12_err_t close_error = F12_OK;
  bool ok = write_pattern_file_chunked(name, file->id, file->size, file->chunk,
                                       &written, &close_error);
  if (!ok && writer_owned)
    writer_abort();
  return ok && written == file->size && close_error == F12_OK;
}

static bool capacity_verify(void) {
  for (uint32_t i = 0; i < CAPACITY_FILE_COUNT; i++) {
    const capacity_file_t *file = &capacity_files[i];
    char name[13];
    capacity_name(file, i, name);
    if (!verify_pattern_file_chunked(name, file->id, file->size, file->chunk,
                                     NULL))
      return false;
  }
  return true;
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

static bool write_blob_file(const char *name, const void *buf, uint32_t size,
                            uint32_t chunk_size, uint32_t *written_out,
                            f12_err_t *close_err_out) {
  if (chunk_size == 0 || chunk_size > WORK_BUF_SIZE) {
    chunk_size = WORK_BUF_SIZE;
  }

  f12_err_t open_error = writer_open(name);
  if (open_error != F12_OK) {
    if (written_out)
      *written_out = 0;
    if (close_err_out)
      *close_err_out = open_error;
    return false;
  }

  uint32_t written = 0;
  while (written < size) {
    uint32_t chunk = size - written;
    if (chunk > chunk_size)
      chunk = chunk_size;

    f12_result_t result =
        f12_write_full(&owned_writer, (const uint8_t *)buf + written, chunk);
    written += (uint32_t)result.count;
    if (result.error != F12_OK || result.count != chunk) {
      f12_err_t abort_error = writer_abort();
      if (written_out)
        *written_out = written;
      if (close_err_out) {
        *close_err_out =
            abort_error == F12_OK
                ? (result.error == F12_OK ? F12_ERR_IO : result.error)
                : abort_error;
      }
      return false;
    }
  }

  if (written_out)
    *written_out = written;
  f12_err_t close_error = writer_commit();
  if (close_err_out)
    *close_err_out = close_error;
  return close_error == F12_OK;
}

static bool read_blob_file(const char *name, void *buf, uint32_t size,
                           uint32_t *read_out) {
  f12_stat_t stat;
  if (f12_stat(&fs, name, &stat) != F12_OK || stat.size != size) {
    if (read_out)
      *read_out = 0;
    return false;
  }
  f12_file_t file;
  if (f12_open(&fs, name, F12_OPEN_READ, &file) != F12_OK) {
    if (read_out)
      *read_out = 0;
    return false;
  }

  f12_result_t result = f12_read_full(&file, buf, size);
  bool ok = result.error == F12_OK && f12_close(&file) == F12_OK;

  if (read_out)
    *read_out = (uint32_t)result.count;
  return ok && result.count == size;
}

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
  f12_err_t close_err = F12_OK;
  return write_blob_file(CRASH_STATE_FILE, state, sizeof(*state), 37, &written,
                         &close_err) &&
         written == sizeof(*state) && close_err == F12_OK;
}

static bool crash_state_load(crash_state_t *state) {
  uint32_t got = 0;
  if (!read_blob_file(CRASH_STATE_FILE, state, sizeof(*state), &got)) {
    return false;
  }

  return got == sizeof(*state) && crash_state_valid(state);
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

  f12_err_t err = f12_opendir(&fs, "/", &dir);
  if (err != F12_OK) {
    printf("Error: %s\n", f12_strerror(err));
    return;
  }

  int count = 0;
  uint32_t total_bytes = 0;
  f12_err_t read_error;
  while ((read_error = f12_readdir(&dir, &st)) == F12_OK) {
    if ((st.attr & FAT12_ATTR_DIRECTORY) != 0)
      printf("  %-12s    <DIR>\n", st.name);
    else
      printf("  %-12s %8lu\n", st.name, st.size);
    total_bytes += st.size;
    count++;
  }
  f12_err_t close_error = f12_closedir(&dir);
  if (read_error != F12_END) {
    printf("Error: %s\n", f12_strerror(read_error));
    return;
  }
  if (close_error != F12_OK) {
    printf("Error: %s\n", f12_strerror(close_error));
    return;
  }

  if (count == 0) {
    printf("  (empty)\n");
  }

  uint16_t free_cl;
  f12_err_t error = f12_free_count(&fs, &free_cl);
  if (error != F12_OK) {
    printf("Free-space query failed: %s\n", f12_strerror(error));
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
  f12_err_t error = f12_open(&fs, name, F12_OPEN_READ, &file);
  if (error != F12_OK) {
    printf("Error: %s\n", f12_strerror(error));
    return;
  }

  int total = 0;
  for (;;) {
    f12_result_t result = f12_read(&file, io_buf, IO_BUF_SIZE);
    if (result.count > IO_BUF_SIZE) {
      printf("\nRead error: invalid transfer count\n");
      break;
    }
    for (size_t i = 0; i < result.count; i++) {
      putchar(io_buf[i]);
    }
    total += (int)result.count;
    if (result.error == F12_END)
      break;
    if (result.error != F12_OK) {
      printf("\nRead error: %s\n", f12_strerror(result.error));
      break;
    }
  }
  printf("\n(%d bytes)\n", total);
  error = f12_close(&file);
  if (error != F12_OK)
    printf("Close error: %s\n", f12_strerror(error));
}

static void cmd_hexdump(int argc, char **argv) {
  (void)argc;
  char name[13];
  if (!resolve_name(argv[1], name)) return;

  f12_file_t file;
  f12_err_t error = f12_open(&fs, name, F12_OPEN_READ, &file);
  if (error != F12_OK) {
    printf("Error: %s\n", f12_strerror(error));
    return;
  }

  uint32_t offset = 0;
  for (;;) {
    f12_result_t result = f12_read(&file, io_buf, 16);
    if (result.count > 16u) {
      printf("Read error: invalid transfer count\n");
      break;
    }
    size_t n = result.count;
    if (n == 0 && result.error == F12_END)
      break;
    if (result.error != F12_OK && result.error != F12_END) {
      printf("Read error: %s\n", f12_strerror(result.error));
      break;
    }
    printf("  %08lX: ", offset);
    for (size_t i = 0; i < 16u; i++) {
      if (i < n)
        printf("%02X ", (unsigned)io_buf[i]);
      else
        printf("   ");
      if (i == 7)
        printf(" ");
    }
    printf(" |");
    for (size_t i = 0; i < n; i++) {
      putchar((io_buf[i] >= 32 && io_buf[i] < 127) ? io_buf[i] : '.');
    }
    printf("|\n");
    offset += (uint32_t)n;
    if (result.error == F12_END)
      break;
  }
  printf("  %lu bytes\n", offset);
  error = f12_close(&file);
  if (error != F12_OK)
    printf("Close error: %s\n", f12_strerror(error));
}

static void cmd_write(int argc, char **argv) {
  (void)argc;
  char name[13];
  if (!resolve_name(argv[1], name)) return;

  f12_err_t error = writer_open(name);
  if (error != F12_OK) {
    printf("Error: %s\n", f12_strerror(error));
    return;
  }

  printf("Enter text (end with . on its own line):\n");
  uint32_t total = 0;
  char line[CMD_BUF_SIZE];
  for (;;) {
    int line_length = cli_readline(line);
    if (line_length == CLI_INPUT_CANCELLED) {
      f12_err_t abort_error = writer_abort();
      printf("Write cancelled.\n");
      if (abort_error != F12_OK) {
        printf("Abort failed and writer remains retained: %s\n",
               f12_strerror(abort_error));
      }
      return;
    }
    if (line_length == CLI_INPUT_OVERFLOW) {
      f12_err_t abort_error = writer_abort();
      printf("Input line too long; write aborted.\n");
      if (abort_error != F12_OK) {
        printf("Abort failed and writer remains retained: %s\n",
               f12_strerror(abort_error));
      }
      return;
    }
    if (strcmp(line, ".") == 0)
      break;

    uint32_t requested = (uint32_t)line_length + 1u;
    line[line_length] = '\n';
    f12_result_t result = f12_write_full(&owned_writer, line, requested);
    total += (uint32_t)result.count;
    if (result.error != F12_OK || result.count != requested) {
      f12_err_t abort_error = writer_abort();
      printf("Write failed after %lu bytes: %s\n", (unsigned long)total,
             f12_strerror(result.error == F12_OK ? F12_ERR_IO : result.error));
      if (abort_error != F12_OK) {
        printf("Abort failed and writer remains retained: %s\n",
               f12_strerror(abort_error));
      }
      return;
    }
  }

  error = writer_commit();
  if (error != F12_OK) {
    printf("Commit failed and writer remains retained: %s\n",
           f12_strerror(error));
    return;
  }
  printf("Wrote %lu bytes to %s\n", (unsigned long)total, name);
}

static void cmd_commit(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (!writer_owned) {
    printf("No pending commit.\n");
    return;
  }
  f12_err_t error = writer_commit();
  if (error == F12_OK)
    printf("Commit complete.\n");
  else
    printf("Commit failed and remains retained: %s\n", f12_strerror(error));
}

static void cmd_rm(int argc, char **argv) {
  (void)argc;
  char name[13];
  if (!resolve_name(argv[1], name)) return;

  f12_err_t err = f12_delete(&fs, name);
  if (err != F12_OK)
    printf("Error: %s\n", f12_strerror(err));
  else
    printf("Deleted %s\n", name);
}

static void cmd_cp(int argc, char **argv) {
  (void)argc;
  char src[13], dst[13];
  if (!canonical_name(argv[1], src) || !canonical_name(argv[2], dst)) {
    printf("Source and destination must be valid 8.3 filenames.\n");
    return;
  }

  f12_file_t reader;
  f12_err_t error = f12_open(&fs, src, F12_OPEN_READ, &reader);
  if (error != F12_OK) {
    printf("Error opening %s: %s\n", src, f12_strerror(error));
    return;
  }

  error = writer_open(dst);
  if (error != F12_OK) {
    printf("Error creating %s: %s\n", dst, f12_strerror(error));
    f12_err_t read_close = f12_close(&reader);
    if (read_close != F12_OK)
      printf("Reader close failed: %s\n", f12_strerror(read_close));
    return;
  }

  uint32_t total = 0;
  f12_err_t copy_error = F12_OK;
  for (;;) {
    f12_result_t read_result = f12_read(&reader, io_buf, IO_BUF_SIZE);
    if (read_result.count > IO_BUF_SIZE) {
      copy_error = F12_ERR_IO;
      break;
    }
    if (read_result.count != 0) {
      f12_result_t write_result =
          f12_write_full(&owned_writer, io_buf, (uint32_t)read_result.count);
      total += (uint32_t)write_result.count;
      if (write_result.error != F12_OK ||
          write_result.count != read_result.count) {
        copy_error =
            write_result.error == F12_OK ? F12_ERR_IO : write_result.error;
        break;
      }
    }
    if (read_result.error == F12_END)
      break;
    if (read_result.error != F12_OK) {
      copy_error = read_result.error;
      break;
    }
  }

  f12_err_t read_close = f12_close(&reader);
  if (copy_error == F12_OK && read_close != F12_OK)
    copy_error = read_close;
  f12_err_t write_close =
      copy_error == F12_OK ? writer_commit() : writer_abort();
  if (write_close != F12_OK)
    copy_error = write_close;
  if (copy_error != F12_OK) {
    printf("Copy error after %lu bytes: %s\n", total, f12_strerror(copy_error));
    if (writer_owned)
      printf("Commit retained; run 'commit' to retry.\n");
    return;
  }
  printf("Copied %lu bytes: %s -> %s\n", total, src, dst);
}

static void cmd_mv(int argc, char **argv) {
  (void)argc;
  char src[13], dst[13];
  if (!canonical_name(argv[1], src) || !canonical_name(argv[2], dst)) {
    printf("Source and destination must be valid 8.3 filenames.\n");
    return;
  }

  f12_err_t err = f12_rename(&fs, src, dst);
  if (err != F12_OK) {
    printf("Error: %s\n", f12_strerror(err));
    return;
  }
  printf("Renamed %s -> %s\n", src, dst);
}

static void cmd_stat(int argc, char **argv) {
  (void)argc;
  char name[13];
  if (!resolve_name(argv[1], name)) return;

  f12_stat_t st;
  f12_err_t err = f12_stat(&fs, name, &st);
  if (err != F12_OK) {
    printf("Error: %s\n", f12_strerror(err));
    return;
  }

  printf("  Name:   %s\n", st.name);
  printf("  Size:   %lu bytes\n", st.size);
  printf("  Attr:   0x%02X", st.attr);
  if (st.attr & FAT12_ATTR_READ_ONLY)
    printf(" RO");
  if (st.attr & FAT12_ATTR_HIDDEN)
    printf(" HID");
  if (st.attr & FAT12_ATTR_SYSTEM)
    printf(" SYS");
  if (st.attr & FAT12_ATTR_VOLUME_ID)
    printf(" VOL");
  if (st.attr & FAT12_ATTR_DIRECTORY)
    printf(" DIR");
  if (st.attr & FAT12_ATTR_ARCHIVE)
    printf(" ARC");
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

static f12_err_t do_format(const char *label, bool full) {
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
    if (text_equal_case(argv[1], "full")) {
      full = true;
    } else {
      label = argv[1];
    }
  } else if (argc == 3) {
    if (!text_equal_case(argv[2], "full")) {
      printf("Format mode must be 'full'.\n");
      return;
    }
    label = argv[1];
    full = true;
  }

  printf("Format disk as \"%s\" (%s)? [y/N] ", label, full ? "full" : "quick");

  if (!cli_confirm())
    return;

  f12_err_t err = unmount_filesystem();
  if (err != F12_OK) {
    printf("Unmount failed: %s\n", f12_strerror(err));
    return;
  }

  err = do_format(label, full);
  if (err != F12_OK) {
    printf("Format error: %s\n", f12_strerror(err));
    return;
  }
  printf("Format complete.\n");

  err = do_mount();
  if (err == F12_OK) {
    printf("Mounted.\n");
  } else {
    printf("Mount error: %s\n", f12_strerror(err));
  }
}

static bool fsck_dirty(const fat12_fsck_t *r) {
  return r->lost_clusters || r->crosslinked || r->loops || r->broken_chains ||
         r->size_mismatches || r->truncated_files || r->duplicate_names ||
         r->fat_mismatch || r->fat_ambiguous || r->incomplete;
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
  if (r->removed_directories)
    printf("  Removed dirs:  %u\n", r->removed_directories);
  if (r->removed_duplicates)
    printf("  Removed dupes: %u\n", r->removed_duplicates);
  if (r->freed_tails)
    printf("  Freed tails:   %u\n", r->freed_tails);
  if (r->freed)
    printf("  Freed:         %u clusters\n", r->freed);
  if (r->incomplete)
    printf("  Scan:          incomplete\n");
}

static void cmd_fsck(int argc, char **argv) {
  bool fix = argc == 2;
  if (fix && !text_equal_case(argv[1], "fix")) {
    printf("Repair mode must be 'fix'.\n");
    return;
  }

  fat12_fsck_t report;
  f12_err_t err = f12_fsck(&fs, &report, fix);
  if (err != F12_OK) {
    printf("Error: %s\n", f12_strerror(err));
    return;
  }

  print_fsck_report(&report);

  if (fix) {
    fat12_fsck_t verification;
    err = f12_fsck(&fs, &verification, false);
    if (err != F12_OK) {
      printf("  Verification failed: %s\n", f12_strerror(err));
      return;
    }
    if (fsck_dirty(&verification)) {
      printf("  Result: repair did not converge\n");
      print_fsck_report(&verification);
      return;
    }
    printf("  Result: repaired and verified clean\n");
  } else if (fsck_dirty(&report)) {
    printf("  Result: DIRTY -- run 'fsck fix' to repair\n");
  } else {
    printf("  Result: clean\n");
  }
}

static void cmd_mount(int argc, char **argv) {
  (void)argc;
  (void)argv;
  f12_err_t err = unmount_filesystem();
  if (err != F12_OK) {
    printf("Unmount failed: %s\n", f12_strerror(err));
    return;
  }

  err = do_mount();
  if (err != F12_OK) {
    printf("Mount error: %s\n", f12_strerror(err));
    return;
  }

  printf("Mounted.\n");

  fat12_fsck_t report;
  f12_err_t check_error = f12_fsck(&fs, &report, false);
  if (check_error != F12_OK) {
    printf("Filesystem check failed: %s\n", f12_strerror(check_error));
  } else if (fsck_dirty(&report)) {
    printf("WARNING: filesystem is not clean. Run 'fsck' for details.\n");
  }
}

static void cmd_unmount(int argc, char **argv) {
  (void)argc;
  (void)argv;
  bool mounted;
  f12_err_t error = f12_is_mounted(&fs, &mounted);
  if (error != F12_OK) {
    printf("Filesystem state check failed: %s\n", f12_strerror(error));
    return;
  }
  if (!mounted) {
    if (writer_owned) {
      error = writer_commit();
      if (error != F12_OK)
        printf("Pending commit failed: %s\n", f12_strerror(error));
    }
    printf("Not mounted.\n");
    return;
  }
  error = writer_commit();
  if (error == F12_OK)
    error = f12_unmount(&fs);
  if (error != F12_OK) {
    printf("Unmount failed: %s\n", f12_strerror(error));
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
  if (!drive_status("Write-protect query",
                    floppy_write_protected(&floppy, &write_protected)) ||
      !drive_status("Media-change query",
                    floppy_disk_changed(&floppy, &changed)) ||
      !drive_status("Track query", floppy_current_track(&floppy, &cylinder)) ||
      !drive_status("Track-zero query",
                    floppy_at_track0(&floppy, &at_track0))) {
    return;
  }
  printf("  Drive:\n");
  printf("    Write protected: %s\n", write_protected ? "YES" : "no");
  printf("    Disk changed:    %s\n", changed ? "YES" : "no");
  printf("    Current track:   %u\n", cylinder);
  printf("    At track 0:      %s\n", at_track0 ? "yes" : "no");
  printf("    Pending commit:  %s\n", writer_owned ? "YES" : "no");

  bool mounted;
  f12_err_t mount_error = f12_is_mounted(&fs, &mounted);
  if (mount_error != F12_OK) {
    printf("  Filesystem state: %s\n", f12_strerror(mount_error));
    return;
  }
  if (!mounted) {
    printf("  Filesystem: not mounted\n");
    return;
  }

  printf("  Geometry:\n");
  printf("    Bytes/sector:     %u\n", DISK_SECTOR_SIZE);
  printf("    Sectors/cluster:  %u\n", FAT12_MAX_CLUSTER_SECTORS);
  printf("    Reserved sectors: %u\n", FAT12_RESERVED_SECTORS);
  printf("    FATs:             %u\n", FAT12_NUM_FATS);
  printf("    Root entries:     %u\n", FAT12_ROOT_ENTRIES);
  printf("    Total sectors:    %u\n", DISK_SECTOR_COUNT);
  printf("    Media descriptor: 0x%02X\n", FAT12_MEDIA_DESCRIPTOR);
  printf("    Sectors/FAT:      %u\n", FAT12_SECTORS_PER_FAT);
  printf("    Sectors/track:    %u\n", DISK_SECTORS_PER_TRACK);
  printf("    Heads:            %u\n", DISK_HEADS);

  uint16_t free_cl;
  f12_err_t error = f12_free_count(&fs, &free_cl);
  if (error != F12_OK) {
    printf("  Free-space query failed: %s\n", f12_strerror(error));
    return;
  }
  uint32_t free_bytes = (uint32_t)free_cl * CLUSTER_SIZE;
  printf("  Free: %lu bytes (%d clusters)\n", free_bytes, free_cl);
}

static void cmd_motor(int argc, char **argv) {
  (void)argc;
  if (text_equal_case(argv[1], "on")) {
    if (drive_status("Motor start", floppy_motor_on(&floppy))) {
      printf("Motor ON\n");
    }
  } else if (text_equal_case(argv[1], "off")) {
    if (drive_status("Motor stop", floppy_motor_off(&floppy))) {
      printf("Motor off\n");
    }
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
  block_status_t st = floppy_seek(&floppy, 0);
  if (!drive_status("Seek", st))
    return;
  bool active;
  if (!drive_status("Track-zero query", floppy_at_track0(&floppy, &active)))
    return;
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
  if (!drive_status("Drive select", floppy_select(&floppy, true)))
    return;
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
  if (transitions == 0)
    printf("  No activity on read_data -- check wiring or disk.\n");
  drive_idle();
}

static void cmd_flux(int argc, char **argv) {
  uint32_t count = 200;
  if (argc == 2 && !parse_u32(argv[1], 1, 10000, &count)) {
    printf("Count must be an integer from 1 through 10000.\n");
    return;
  }

  printf("  Reading %lu raw flux transitions...\n", (unsigned long)count);
  printf("  read_data=GP%u  index=GP%u\n", drive_pins.read_data,
         drive_pins.index);

  uint32_t generation;
  if (!drive_generation(&generation) ||
      !drive_status("Flux capture", floppy_flux_begin(&floppy, generation)))
    return;

  for (uint32_t i = 0; i < count; i++) {
    uint16_t delta;
    bool ix;
    block_status_t status = floppy_flux_next(&floppy, &delta, &ix);
    if (status != BLOCK_OK) {
      printf("  Capture stopped after %lu transitions: %s.\n", (unsigned long)i,
             block_status_text(status));
      printf("  Check: disk inserted? read_data wiring? motor spinning?\n");
      printf("  Current read_data (GP%u) = %d\n", drive_pins.read_data,
             gpio_get(drive_pins.read_data));
      drive_status("Flux stop", floppy_flux_end(&floppy));
      return;
    }

    printf("  %4lu: delta=%3u  ix=%d\n", (unsigned long)i, delta, ix);
  }

  if (!drive_status("Flux stop", floppy_flux_end(&floppy)))
    return;
  printf("  Done.\n");
}

static void cmd_seek(int argc, char **argv) {
  (void)argc;
  uint32_t cylinder;
  if (!parse_u32(argv[1], 0, DISK_CYLINDERS - 1u, &cylinder)) {
    printf("Cylinder must be 0 through %u.\n", DISK_CYLINDERS - 1u);
    return;
  }
  block_status_t st = floppy_seek(&floppy, (uint8_t)cylinder);
  if (drive_status("Seek", st))
    printf("Head at cylinder %lu\n", (unsigned long)cylinder);
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

  uint8_t sector[DISK_SECTOR_SIZE];
  uint32_t generation;
  if (!drive_generation(&generation))
    return;
  for (uint32_t sector_number = sec_start; sector_number <= sec_end;
       sector_number++) {
    block_status_t st = floppy_read_sector(
        &floppy, generation, (uint8_t)cylinder, (uint8_t)head,
        (uint8_t)(sector_number - 1u), sector);
    printf("  --- C%lu/H%lu/S%lu %s ---\n", (unsigned long)cylinder,
           (unsigned long)head, (unsigned long)sector_number,
           block_status_text(st));

    if (st != BLOCK_OK)
      continue;

    for (int row = 0; row < 32; row++) {
      int off = row * 16;
      printf("  %03X: ", off);
      for (int i = 0; i < 16; i++) {
        printf("%02X ", sector[off + i]);
        if (i == 7)
          printf(" ");
      }
      printf(" |");
      for (int i = 0; i < 16; i++) {
        uint8_t c = sector[off + i];
        putchar((c >= 32 && c < 127) ? c : '.');
      }
      printf("|\n");
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
  track_stats_t stats;
  read_track_stats((int)cylinder, (int)head, &stats);

  printf("  Pulses:   %lu total\n", stats.total_pulses);
  printf("  Short:    %lu (%.1f%%)\n", stats.short_count,
         stats.total_pulses ? stats.short_count * 100.0 / stats.total_pulses
                            : 0);
  printf("  Medium:   %lu (%.1f%%)\n", stats.medium_count,
         stats.total_pulses ? stats.medium_count * 100.0 / stats.total_pulses
                            : 0);
  printf("  Long:     %lu (%.1f%%)\n", stats.long_count,
         stats.total_pulses ? stats.long_count * 100.0 / stats.total_pulses
                            : 0);
  printf("  Invalid:  %lu (%.1f%%)\n", stats.invalid_count,
         stats.total_pulses ? stats.invalid_count * 100.0 / stats.total_pulses
                            : 0);
  printf("  Syncs:    %lu\n", stats.syncs);
  printf("  Records:  %lu\n", stats.sector_records);
  printf("  Unique:   %lu / %d\n", stats.unique_sectors,
         DISK_SECTORS_PER_TRACK);
  printf("  CRC err:  %lu\n", stats.crc_errors);
  printf("  Adaptive: T2_max=%d  T3_max=%d\n", stats.T2_max, stats.T3_max);

  print_histogram(&stats);
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
  f12_stat_t stat;
  if (f12_stat(&fs, name, &stat) != F12_OK || stat.size != expected)
    return false;
  f12_file_t file;
  if (f12_open(&fs, name, F12_OPEN_READ, &file) != F12_OK)
    return false;
  f12_result_t result = f12_read_full(&file, buf, expected);
  bool close_ok = f12_close(&file) == F12_OK;
  return close_ok && result.error == F12_OK && result.count == expected;
}

static void mfm_health_check(int track, int side, const char *label, int *pass,
                             int *fail) {
  track_stats_t stats;
  read_track_stats(track, side, &stats);

  printf("  %s T%d/S%d: unique=%lu/%d records=%lu crc=%lu invalid=%lu/%lu\n",
         label, track, side, stats.unique_sectors, DISK_SECTORS_PER_TRACK,
         stats.sector_records, stats.crc_errors, stats.invalid_count,
         stats.total_pulses);

  uint32_t invalid_limit = stats.total_pulses / 200;
  if (invalid_limit < 1)
    invalid_limit = 1;

  char tag[96];
  snprintf(tag, sizeof(tag), "%s unique sectors", label);
  check(stats.unique_sectors == DISK_SECTORS_PER_TRACK, tag, pass, fail);
  snprintf(tag, sizeof(tag), "%s CRC clean", label);
  check(stats.crc_errors == 0, tag, pass, fail);
  snprintf(tag, sizeof(tag), "%s invalid pulse threshold", label);
  check(stats.invalid_count <= invalid_limit, tag, pass, fail);
}

static void cmd_test_full(int argc, char **argv) {
  uint32_t rounds = 6;
  if (argc == 2) {
    if (!parse_u32(argv[1], 1, 100, &rounds)) {
      printf("Rounds must be an integer from 1 through 100.\n");
      return;
    }
  }

  printf("This will FORMAT the disk, run diagnostics, file I/O, and a full "
         "sector scan.\n");
  printf("Continue? [y/N] ");

  if (!cli_confirm())
    return;

  f12_err_t start_error = unmount_filesystem();
  if (start_error != F12_OK) {
    printf("Cannot start test: %s\n", f12_strerror(start_error));
    return;
  }

  int pass = 0;
  int fail = 0;
  f12_err_t cleanup;

  printf("\n--- Phase 1: GPIO and Flux Sanity ---\n");
  cmd_pins(0, NULL);
  cmd_status(0, NULL);
  block_status_t home_status = floppy_seek(&floppy, 0);
  check(home_status == BLOCK_OK, "home and clear media latch", &pass, &fail);
  uint32_t test_generation = 0;
  block_status_t generation_status =
      floppy_media_generation(&floppy, &test_generation);
  bool generation_ready = generation_status == BLOCK_OK && test_generation != 0;
  check(generation_ready, "capture nonzero media generation", &pass, &fail);
  bool stats_reset = floppy_stats_reset(&floppy) == BLOCK_OK;
  check(stats_reset, "reset drive statistics", &pass, &fail);

  block_status_t flux_status = generation_ready
                                   ? floppy_flux_begin(&floppy, test_generation)
                                   : BLOCK_ERR_INVALID;
  check(flux_status == BLOCK_OK, "raw flux session starts", &pass, &fail);
  if (flux_status == BLOCK_OK) {
    check(floppy_seek(&floppy, 1) == BLOCK_ERR_BUSY,
          "raw flux excludes head motion", &pass, &fail);
    check(floppy_deinit(&floppy) == BLOCK_ERR_BUSY,
          "raw flux excludes teardown", &pass, &fail);
    check(floppy_flux_end(&floppy) == BLOCK_OK, "raw flux session stops", &pass,
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
      generation_status == BLOCK_OK && current_generation == test_generation;
  check(generation_stable, "media generation remains stable", &pass, &fail);

  if (home_status != BLOCK_OK || !generation_ready || !generation_stable ||
      !stats_reset || !protection_ok || write_protected)
    goto done;

  printf("\n--- Phase 2: Full Format and Mount ---\n");
  f12_err_t err = do_format("TESTFULL", true);
  check(err == F12_OK, "full format", &pass, &fail);
  if (err != F12_OK)
    goto done;

  err = do_mount();
  check(err == F12_OK, "mount after full format", &pass, &fail);
  if (err != F12_OK)
    goto done;
  cmd_status(0, NULL);

  printf("\n--- Phase 3: MFM Signal Checks ---\n");
  mfm_health_check(0, 0, "outer", &pass, &fail);
  mfm_health_check(DISK_CYLINDERS - 1, 0, "inner", &pass, &fail);

  printf("\n--- Phase 4: Shell File Operations ---\n");
  const char text[] = "test-full hardware sequence\nformat mount write copy "
                      "move delete verify\n";
  uint32_t text_len = sizeof(text) - 1;
  f12_err_t open_error = writer_open("TEST.TXT");
  check(open_error == F12_OK, "open TEST.TXT for write", &pass, &fail);
  if (open_error == F12_OK) {
    f12_result_t write_result = f12_write_full(&owned_writer, text, text_len);
    f12_err_t close_err =
        write_result.error == F12_OK ? writer_commit() : writer_abort();
    check(write_result.error == F12_OK && write_result.count == text_len &&
              close_err == F12_OK,
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
  check(f12_stat(&fs, "COPY.TXT", &st) == F12_ERR_NOT_FOUND,
        "COPY.TXT removed by mv", &pass, &fail);
  exact = read_file_exact("MOVED.TXT", work_buf, text_len);
  check(exact && memcmp(work_buf, text, text_len) == 0, "MOVED.TXT verified",
        &pass, &fail);

  char *rm_args[] = {"rm", "MOVED.TXT"};
  cmd_rm(2, rm_args);
  check(f12_stat(&fs, "MOVED.TXT", &st) == F12_ERR_NOT_FOUND, "rm MOVED.TXT",
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
    f12_err_t close_err = F12_OK;
    bool ok =
        write_pattern_file_chunked(files[i].name, files[i].id, files[i].size,
                                   files[i].chunk, &written, &close_err);
    char tag[80];
    snprintf(tag, sizeof(tag), "write %s", files[i].name);
    check(ok && written == files[i].size && close_err == F12_OK, tag, &pass,
          &fail);
  }

  for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    char tag[80];
    snprintf(tag, sizeof(tag), "verify %s", files[i].name);
    check(verify_pattern_file_chunked(files[i].name, files[i].id, files[i].size,
                                      files[i].chunk, NULL),
          tag, &pass, &fail);
  }

  for (uint32_t round = 0; round < rounds; round++) {
    uint32_t size = 2048 + (round % 9u) * 1536;
    uint32_t chunk = 1 + (round % 7u) * 73;
    uint32_t id = 2000u + round;
    uint32_t written = 0;
    f12_err_t close_err = F12_OK;
    bool ok = write_pattern_file_chunked("ROUND.BIN", id, size, chunk, &written,
                                         &close_err);
    char tag[96];
    snprintf(tag, sizeof(tag), "round %lu overwrite write",
             (unsigned long)round + 1u);
    check(ok && written == size && close_err == F12_OK, tag, &pass, &fail);

    snprintf(tag, sizeof(tag), "round %lu overwrite verify",
             (unsigned long)round + 1u);
    check(verify_pattern_file_chunked("ROUND.BIN", id, size, chunk + 31, NULL),
          tag, &pass, &fail);
  }

  err = f12_delete(&fs, "SMALL.BIN");
  check(err == F12_OK, "delete SMALL.BIN", &pass, &fail);
  err = f12_delete(&fs, "MEDIUM.BIN");
  check(err == F12_OK, "delete MEDIUM.BIN", &pass, &fail);
  check(f12_stat(&fs, "SMALL.BIN", &st) == F12_ERR_NOT_FOUND, "SMALL.BIN gone",
        &pass, &fail);
  check(f12_stat(&fs, "MEDIUM.BIN", &st) == F12_ERR_NOT_FOUND,
        "MEDIUM.BIN gone", &pass, &fail);

  uint32_t written = 0;
  f12_err_t close_err = F12_OK;
  bool ok = write_pattern_file_chunked("REFILL.BIN", 3000, 12000, 333, &written,
                                       &close_err);
  check(ok && written == 12000 && close_err == F12_OK, "write REFILL.BIN",
        &pass, &fail);
  check(verify_pattern_file_chunked("REFILL.BIN", 3000, 12000, 777, NULL),
        "verify REFILL.BIN", &pass, &fail);

  printf("\n--- Phase 6: Full Capacity and Fragmentation ---\n");
  uint16_t free_clusters = 0;
  err = f12_free_count(&fs, &free_clusters);
  bool capacity_ready = err == F12_OK && free_clusters >= CAPACITY_FILE_COUNT;
  check(capacity_ready, "capacity workload has enough free clusters", &pass,
        &fail);
  if (!capacity_ready)
    goto done;

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
  if (!capacity_ok || remaining_clusters != 0u)
    goto done;

  free_clusters = 1u;
  err = f12_free_count(&fs, &free_clusters);
  check(err == F12_OK && free_clusters == 0u, "disk reports zero free space",
        &pass, &fail);
  if (err != F12_OK || free_clusters != 0u)
    goto done;

  bool capacity_exact = capacity_verify();
  check(capacity_exact, "all randomized full-disk files are exact", &pass,
        &fail);
  if (!capacity_exact)
    goto done;

  f12_err_t overflow_open = writer_open("OVERFLOW.BIN");
  f12_result_t overflow_write = {.error = F12_ERR_INVALID, .count = 0};
  if (overflow_open == F12_OK) {
    work_buf[0] = gen_pattern_byte(0xFFFFFFFFu, 0);
    overflow_write = f12_write_full(&owned_writer, work_buf, 1);
  }
  f12_err_t overflow_abort = writer_abort();
  bool overflow_rejected =
      overflow_open == F12_OK && overflow_write.error == F12_ERR_FULL &&
      overflow_write.count == 0u && overflow_abort == F12_OK;
  check(overflow_rejected, "new-file write reports disk full", &pass, &fail);
  if (!overflow_rejected)
    goto done;

  free_clusters = 1u;
  err = f12_free_count(&fs, &free_clusters);
  bool overflow_absent =
      f12_stat(&fs, "OVERFLOW.BIN", &st) == F12_ERR_NOT_FOUND;
  check(err == F12_OK && free_clusters == 0u && overflow_absent,
        "failed full-disk file remains unpublished", &pass, &fail);
  if (err != F12_OK || free_clusters != 0u || !overflow_absent)
    goto done;

  const capacity_file_t *preserved = &capacity_files[0];
  char preserved_name[13];
  capacity_name(preserved, 0, preserved_name);
  f12_err_t replace_open = writer_open(preserved_name);
  f12_result_t replace_write = {.error = F12_ERR_INVALID, .count = 0};
  if (replace_open == F12_OK) {
    work_buf[0] =
        (uint8_t)(gen_pattern_byte(preserved->id, 0) ^ (uint8_t)0xFFu);
    replace_write = f12_write_full(&owned_writer, work_buf, 1);
  }
  f12_err_t replace_abort = writer_abort();
  bool replace_rejected = replace_open == F12_OK &&
                          replace_write.error == F12_ERR_FULL &&
                          replace_write.count == 0u && replace_abort == F12_OK;
  check(replace_rejected, "copy-on-write write reports disk full", &pass,
        &fail);
  check(replace_rejected && verify_pattern_file_chunked(
                                preserved_name, preserved->id, preserved->size,
                                preserved->chunk, NULL),
        "failed replacement preserves original file", &pass, &fail);
  if (!replace_rejected)
    goto done;

  uint32_t freed_clusters = 0;
  uint32_t deleted_files = 0;
  bool delete_ok = true;
  for (uint32_t i = 1u; i < CAPACITY_FILE_COUNT; i += 3u) {
    char name[13];
    capacity_name(&capacity_files[i], i, name);
    if (f12_delete(&fs, name) != F12_OK) {
      delete_ok = false;
      break;
    }
    freed_clusters += capacity_cluster_count(capacity_files[i].size);
    deleted_files++;
  }
  check(delete_ok, "delete randomized files across the full disk", &pass,
        &fail);
  if (!delete_ok)
    goto done;

  free_clusters = 0;
  err = f12_free_count(&fs, &free_clusters);
  check(err == F12_OK && free_clusters == freed_clusters,
        "fragmented free-space count is exact", &pass, &fail);
  if (err != F12_OK || free_clusters != freed_clusters)
    goto done;

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
  if (!refill_ok || refill_number != deleted_files || remaining_clusters != 0u)
    goto done;

  free_clusters = 1u;
  err = f12_free_count(&fs, &free_clusters);
  check(err == F12_OK && free_clusters == 0u,
        "fragmented refill returns disk to full", &pass, &fail);
  if (err != F12_OK || free_clusters != 0u)
    goto done;

  printf("\n--- Phase 7: Remount Verify ---\n");
  err = unmount_filesystem();
  if (err == F12_OK)
    err = do_mount();
  check(err == F12_OK, "remount after full-capacity stress", &pass, &fail);
  if (err != F12_OK)
    goto done;
  check(verify_pattern_file_chunked("BYTEIO.BIN", 1000, 4096, 1, NULL),
        "BYTEIO.BIN survives remount", &pass, &fail);
  check(verify_pattern_file_chunked("LARGE.BIN", 1003, 32768, 4096, NULL),
        "LARGE.BIN survives remount", &pass, &fail);
  check(verify_pattern_file_chunked("REFILL.BIN", 3000, 12000, 777, NULL),
        "REFILL.BIN survives remount", &pass, &fail);
  check(capacity_verify(), "all fragmented files survive remount", &pass,
        &fail);
  free_clusters = 1u;
  err = f12_free_count(&fs, &free_clusters);
  check(err == F12_OK && free_clusters == 0u, "full capacity survives remount",
        &pass, &fail);

  fat12_fsck_t report;
  err = f12_fsck(&fs, &report, false);
  check(err == F12_OK && !fsck_dirty(&report),
        "filesystem is clean after remount", &pass, &fail);

  floppy_stats_t write_stats;
  block_status_t stats_status = floppy_stats(&floppy, &write_stats);
  check(stats_status == BLOCK_OK, "query write-path statistics", &pass, &fail);
  if (stats_status == BLOCK_OK) {
    check(write_stats.dma_writes != 0, "verified DMA track writes observed",
          &pass, &fail);
    check(write_stats.underruns == 0 && write_stats.overruns == 0,
          "no write or read flow faults", &pass, &fail);
    check(write_stats.media_changes == 0, "no unexpected media changes", &pass,
          &fail);
  }
  current_generation = 0;
  generation_status = floppy_media_generation(&floppy, &current_generation);
  check(generation_status == BLOCK_OK && current_generation == test_generation,
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
  cleanup = unmount_filesystem();
  if (cleanup != F12_OK) {
    printf("Cleanup failed: %s\n", f12_strerror(cleanup));
    fail++;
  }
  if (!drive_idle())
    fail++;

  printf("\n=== Test-Full Complete ===\n");
  printf("  Checks: %d passed, %d failed\n", pass, fail);
  printf("  Result: %s\n", fail == 0 ? "ALL PASSED" : "SOME FAILED");
}

static void cmd_test_media(int argc, char **argv) {
  (void)argc;
  (void)argv;

  f12_err_t error = writer_commit();
  if (error != F12_OK) {
    printf("Cannot start test: %s\n", f12_strerror(error));
    return;
  }

  block_status_t motor_status = floppy_motor_off(&floppy);
  block_status_t select_status = floppy_select(&floppy, true);
  if (motor_status != BLOCK_OK || select_status != BLOCK_OK) {
    drive_status("Motor stop", motor_status);
    drive_status("Drive select", select_status);
    drive_idle();
    return;
  }

  f12_dir_t stale_dir;
  error = f12_opendir(&fs, "/", &stale_dir);
  if (error != F12_OK) {
    printf("Cannot open stale-handle probe: %s\n", f12_strerror(error));
    drive_idle();
    return;
  }

  uint32_t original_generation = 0;
  block_status_t generation_status =
      floppy_media_generation(&floppy, &original_generation);
  if (generation_status != BLOCK_OK || original_generation == 0) {
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
  block_status_t changed_status = floppy_disk_changed(&floppy, &changed);
  uint32_t replacement_generation = 0;
  generation_status = floppy_media_generation(&floppy, &replacement_generation);
  uint32_t expected_generation = original_generation + 1u;
  if (expected_generation == 0)
    expected_generation = 1u;
  bool generation_changed = generation_status == BLOCK_OK &&
                            replacement_generation == expected_generation;
  check(changed_status == BLOCK_OK && changed, "disk-change latch asserted",
        &pass, &fail);
  check(generation_changed, "media generation advanced exactly by event", &pass,
        &fail);

  track_t stale_track = {.cylinder = 0, .head = 0};
  block_status_t stale_read =
      floppy_read_track(&floppy, original_generation, &stale_track);
  check(stale_read == BLOCK_ERR_MEDIA_CHANGED,
        "stale hardware generation is rejected", &pass, &fail);

  bool mounted = true;
  f12_err_t state_error = f12_is_mounted(&fs, &mounted);
  check(state_error == F12_ERR_MEDIA_CHANGED && !mounted,
        "mounted filesystem observes media change", &pass, &fail);
  f12_err_t stale_close = f12_closedir(&stale_dir);
  check(stale_close == F12_ERR_BAD_HANDLE,
        "stale filesystem handle is invalidated", &pass, &fail);

  if (!generation_changed || state_error != F12_ERR_MEDIA_CHANGED)
    goto done;

  printf("Insert another canonical 1.44 MB FAT12 disk, then enter y.\n");
  printf("Continue? [y/N] ");
  if (!cli_confirm()) {
    drive_idle();
    return;
  }

  block_status_t home_status = floppy_seek(&floppy, 0);
  check(home_status == BLOCK_OK, "replacement disk homes successfully", &pass,
        &fail);
  changed = true;
  changed_status = floppy_disk_changed(&floppy, &changed);
  check(changed_status == BLOCK_OK && !changed,
        "disk-change latch clears after physical step", &pass, &fail);
  if (home_status != BLOCK_OK || changed_status != BLOCK_OK || changed)
    goto done;

  error = do_mount();
  check(error == F12_OK, "replacement filesystem mounts", &pass, &fail);
  if (error == F12_OK) {
    fat12_fsck_t report;
    error = f12_fsck(&fs, &report, false);
    check(error == F12_OK && !fsck_dirty(&report),
          "replacement filesystem is clean", &pass, &fail);
  }

done:
  mounted = false;
  f12_err_t cleanup = f12_is_mounted(&fs, &mounted);
  if (cleanup == F12_OK && mounted)
    cleanup = f12_unmount(&fs);
  if (cleanup != F12_OK) {
    printf("Filesystem cleanup failed: %s\n", f12_strerror(cleanup));
    fail++;
  }
  if (!drive_idle())
    fail++;

  printf("\n=== Test-Media Complete ===\n");
  printf("  Checks: %d passed, %d failed\n", pass, fail);
  printf("  Result: %s\n", fail == 0 ? "ALL PASSED" : "SOME FAILED");
}

static void cmd_rpm(int argc, char **argv) {
  (void)argc;
  (void)argv;

  if (!drive_status("Drive select", floppy_select(&floppy, true)))
    return;
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
      if (have_last) {
        periods[got++] = (uint32_t)absolute_time_diff_us(last, t);
      }
      last = t;
      have_last = true;
    }
    prev = now;
  }

  uint32_t sum = 0, min = ~0u, max = 0;
  for (int i = 0; i < 5; i++) {
    sum += periods[i];
    if (periods[i] < min)
      min = periods[i];
    if (periods[i] > max)
      max = periods[i];
  }
  uint32_t avg = sum / 5;
  uint32_t rpm_x10 = avg ? 600000000u / avg : 0;
  int32_t dev_pm = avg ? (int32_t)(((int64_t)avg - 200000) * 1000 / 200000) : 0;
  int32_t dev_abs = dev_pm < 0 ? -dev_pm : dev_pm;

  printf("  Period:    avg %lu us (min %lu, max %lu, spread %lu)\n", avg, min,
         max, max - min);
  printf("  Speed:     %lu.%lu RPM (nominal 300.0)\n", rpm_x10 / 10,
         rpm_x10 % 10);
  printf("  Deviation: %s%ld.%ld%%\n", dev_pm < 0 ? "-" : "+", dev_abs / 10,
         dev_abs % 10);
  if (dev_abs > 50) {
    printf(
        "  WARNING: more than 5%% off nominal -- writes may be unreliable\n");
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

  if (!cli_confirm())
    return;

  f12_err_t unmount_error = unmount_filesystem();
  if (unmount_error != F12_OK) {
    printf("Cannot start test: %s\n", f12_strerror(unmount_error));
    return;
  }

  f12_err_t error = do_format("CRASH", false);
  if (error != F12_OK) {
    printf("Format failed: %s\n", f12_strerror(error));
    return;
  }
  error = do_mount();
  if (error != F12_OK) {
    printf("Mount failed: %s\n", f12_strerror(error));
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
  f12_err_t close_error = F12_OK;
  if (!write_pattern_file_chunked(CRASH_STABLE_FILE, state.stable_id,
                                  state.stable_size, DISK_SECTOR_SIZE, &written,
                                  &close_error) ||
      written != state.stable_size) {
    printf("Stable-file write failed: %s\n", f12_strerror(close_error));
    goto cleanup;
  }
  if (!write_pattern_file_chunked(CRASH_TARGET_FILE, state.target_old_id,
                                  state.target_size, DISK_SECTOR_SIZE, &written,
                                  &close_error) ||
      written != state.target_size) {
    printf("Baseline target write failed: %s\n", f12_strerror(close_error));
    goto cleanup;
  }

  uint16_t available = 0;
  error = f12_free_count(&fs, &available);
  uint32_t target_clusters =
      (state.target_size + CLUSTER_SIZE - 1u) / CLUSTER_SIZE;
  if (error != F12_OK) {
    printf("Free-space query failed: %s\n", f12_strerror(error));
    goto cleanup;
  }
  if (available <= target_clusters + 1u) {
    printf("Disk has %u free clusters; the test requires more than %lu.\n",
           available, (unsigned long)(target_clusters + 1u));
    goto cleanup;
  }
  state.filler_size =
      ((uint32_t)available - target_clusters - 1u) * CLUSTER_SIZE;
  if (!crash_state_store(&state)) {
    printf("Crash-state write failed.\n");
    goto cleanup;
  }
  if (!write_pattern_file_chunked(CRASH_FILLER_FILE, state.filler_id,
                                  state.filler_size, 8u * DISK_SECTOR_SIZE,
                                  &written, &close_error) ||
      written != state.filler_size) {
    printf("Filler write failed: %s\n", f12_strerror(close_error));
    goto cleanup;
  }

  printf("Prepared stable=%lu, target=%lu, filler=%lu bytes.\n",
         (unsigned long)state.stable_size, (unsigned long)state.target_size,
         (unsigned long)state.filler_size);
  printf("Pull power during an overwrite. Press any key after a completed "
         "round to stop.\n");

  for (uint32_t round = 1;; round++) {
    uint32_t pattern = (round & 1u) ? state.target_new_id : state.target_old_id;
    bool ok = write_pattern_file_chunked(CRASH_TARGET_FILE, pattern,
                                         state.target_size, DISK_SECTOR_SIZE,
                                         &written, &close_error);
    if (!ok || written != state.target_size) {
      printf("Round %lu failed: %s\n", (unsigned long)round,
             f12_strerror(close_error));
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
  if (error != F12_OK)
    printf("Filesystem cleanup failed: %s\n", f12_strerror(error));
  drive_idle();
}

static void cmd_crashcheck(int argc, char **argv) {
  (void)argc;
  (void)argv;

  f12_err_t unmount_error = unmount_filesystem();
  if (unmount_error != F12_OK) {
    printf("Cannot remount: %s\n", f12_strerror(unmount_error));
    return;
  }

  int pass = 0;
  int fail = 0;
  f12_err_t cleanup_error;
  f12_err_t error = do_mount();
  check(error == F12_OK, "disk mounts after power cut", &pass, &fail);
  if (error != F12_OK) {
    printf("Run 'crashtest' first.\n");
    goto cleanup;
  }

  crash_state_t state;
  bool state_valid = crash_state_load(&state);
  check(state_valid, "crash state is exact and checksummed", &pass, &fail);
  if (!state_valid)
    goto cleanup;

  fat12_fsck_t report;
  error = f12_fsck(&fs, &report, false);
  check(error == F12_OK, "fsck check completes", &pass, &fail);
  if (error != F12_OK)
    goto cleanup;
  print_fsck_report(&report);

  error = f12_fsck(&fs, &report, true);
  check(error == F12_OK, "fsck repair completes", &pass, &fail);
  if (error != F12_OK)
    goto cleanup;
  error = f12_fsck(&fs, &report, false);
  check(error == F12_OK && !fsck_dirty(&report), "fsck repair converges", &pass,
        &fail);
  if (error != F12_OK || fsck_dirty(&report))
    goto cleanup;

  check(verify_pattern_file_chunked(CRASH_STABLE_FILE, state.stable_id,
                                    state.stable_size, DISK_SECTOR_SIZE, NULL),
        "stable file is exact", &pass, &fail);
  check(verify_pattern_file_chunked(CRASH_FILLER_FILE, state.filler_id,
                                    state.filler_size, 8u * DISK_SECTOR_SIZE,
                                    NULL),
        "filler file is exact", &pass, &fail);

  bool old_exact =
      verify_pattern_file_chunked(CRASH_TARGET_FILE, state.target_old_id,
                                  state.target_size, DISK_SECTOR_SIZE, NULL);
  bool new_exact =
      verify_pattern_file_chunked(CRASH_TARGET_FILE, state.target_new_id,
                                  state.target_size, DISK_SECTOR_SIZE, NULL);
  check(old_exact != new_exact, "target is exactly one recorded generation",
        &pass, &fail);

cleanup:
  cleanup_error = unmount_filesystem();
  if (cleanup_error != F12_OK) {
    printf("Filesystem cleanup failed: %s\n", f12_strerror(cleanup_error));
    fail++;
  }
  if (!drive_idle())
    fail++;

  printf("\n=== Crashcheck Complete ===\n");
  printf("  Checks: %d passed, %d failed\n", pass, fail);
  printf("  Result: %s\n", fail == 0 ? "ALL PASSED" : "SOME FAILED");
}

static diskdump_stats_t run_diskdump(bool verbose) {
  diskdump_stats_t stats;
  memset(&stats, 0, sizeof(stats));

  int total_valid = 0;
  int total_invalid = 0;
  uint32_t disk_checksum = 5381u;
  uint8_t sector[DISK_SECTOR_SIZE];
  uint32_t generation;
  if (!drive_generation(&generation))
    return stats;

  if (!drive_status("Statistics reset", floppy_stats_reset(&floppy))) {
    return stats;
  }

  if (verbose) {
    printf("  %-8s %-6s %-10s %-10s\n", "TRACK", "SIDE", "DECODED", "ERRORS");
    printf("  %-8s %-6s %-10s %-10s\n", "-----", "----", "-------", "------");
  }

  for (uint8_t track = 0; track < DISK_CYLINDERS; track++) {
    for (uint8_t side = 0; side < DISK_HEADS; side++) {
      int decoded = 0;
      int errors = 0;

      for (uint8_t sector_number = 0; sector_number < DISK_SECTORS_PER_TRACK;
           sector_number++) {
        block_status_t status = floppy_read_sector(&floppy, generation, track,
                                                   side, sector_number, sector);
        if (status == BLOCK_OK) {
          decoded++;
          disk_checksum =
              checksum_extend(disk_checksum, sector, DISK_SECTOR_SIZE);
        } else {
          errors++;
        }
      }

      total_valid += decoded;
      total_invalid += errors;

      if (verbose || decoded != DISK_SECTORS_PER_TRACK || errors != 0) {
        printf("  T%02u      %u      %2d/%-2u      %d\n", track, side, decoded,
               DISK_SECTORS_PER_TRACK, errors);
      }
    }
  }

  stats.total_valid = total_valid;
  stats.total_invalid = total_invalid;
  stats.checksum = disk_checksum;
  if (!drive_status("Statistics query",
                    floppy_stats(&floppy, &stats.retries))) {
    stats.total_invalid++;
  }

  printf("\n  Total decoded: %d / %u\n", total_valid, DISK_SECTOR_COUNT);
  printf("  Errors:        %d\n", total_invalid);
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

  track_stats_t stats;

  struct {
    uint8_t track;
    uint8_t side;
    const char *label;
  } targets[] = {
      {0, 0, "outermost"},
      {(DISK_CYLINDERS / 2u) - 1u, 0, "middle"},
      {DISK_CYLINDERS - 1u, 0, "innermost"},
  };

  for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
    printf("\n  === %s cylinder %u ===\n", targets[t].label, targets[t].track);
    read_track_stats(targets[t].track, targets[t].side, &stats);

    printf("    Pulses:   %lu total\n", stats.total_pulses);
    printf("    Short:    %lu (%.1f%%)\n", stats.short_count,
           stats.total_pulses ? stats.short_count * 100.0 / stats.total_pulses
                              : 0);
    printf("    Medium:   %lu (%.1f%%)\n", stats.medium_count,
           stats.total_pulses ? stats.medium_count * 100.0 / stats.total_pulses
                              : 0);
    printf("    Long:     %lu (%.1f%%)\n", stats.long_count,
           stats.total_pulses ? stats.long_count * 100.0 / stats.total_pulses
                              : 0);
    printf("    Invalid:  %lu (%.1f%%)\n", stats.invalid_count,
           stats.total_pulses ? stats.invalid_count * 100.0 / stats.total_pulses
                              : 0);
    printf("    Syncs:    %lu\n", stats.syncs);
    printf("    Records:  %lu\n", stats.sector_records);
    printf("    Unique:   %lu / %d\n", stats.unique_sectors,
           DISK_SECTORS_PER_TRACK);
    printf("    CRC err:  %lu\n", stats.crc_errors);
    printf("    Adaptive: T2_max=%d  T3_max=%d\n", stats.T2_max, stats.T3_max);
    print_histogram(&stats);
  }

  printf("\n  === Per-Track Summary (side 0) ===\n");
  printf("  %-6s %-8s %-8s %-8s %-8s %-5s %-5s %-5s\n", "TRACK", "SHORT",
         "MEDIUM", "LONG", "INVALID", "UNIQ", "REC", "CRC");
  printf("  %-6s %-8s %-8s %-8s %-8s %-5s %-5s %-5s\n", "-----", "------",
         "------", "------", "-------", "----", "---", "---");

  uint32_t total_sectors = 0;
  uint32_t total_crc = 0;

  for (uint8_t track = 0; track < DISK_CYLINDERS; track++) {
    read_track_stats(track, 0, &stats);
    printf("  T%02u    %-8lu %-8lu %-8lu %-8lu %-5lu %-5lu %-5lu\n", track,
           stats.short_count, stats.medium_count, stats.long_count,
           stats.invalid_count, stats.unique_sectors, stats.sector_records,
           stats.crc_errors);
    total_sectors += stats.unique_sectors;
    total_crc += stats.crc_errors;
  }

  printf("\n  Head 0 total: %lu unique sectors decoded, %lu CRC errors\n",
         (unsigned long)total_sectors, (unsigned long)total_crc);
}

static void cmd_version(int argc, char **argv) {
  (void)argc;
  (void)argv;
#if defined(PICO_RP2040) && PICO_RP2040
  const char *board = "RP2040 (Pico)";
#elif defined(PICO_RP2350) && PICO_RP2350
  const char *board = "RP2350 (Pico 2)";
#else
#error unsupported Raspberry Pi silicon
#endif
  printf("Pico Floppy %s\n", FW_VERSION);
  printf("  board:     %s\n", board);
  printf("  sys clock: %lu MHz\n",
         (unsigned long)(clock_get_hz(clk_sys) / 1000000));
}

static void cmd_reboot(int argc, char **argv) {
  (void)argc;
  (void)argv;
  f12_err_t error = unmount_filesystem();
  if (error != F12_OK) {
    printf("Reboot refused: %s\n", f12_strerror(error));
    return;
  }
  block_status_t drive_error = floppy_deinit(&floppy);
  if (drive_error != BLOCK_OK) {
    printf("Reboot refused: %s\n", block_status_text(drive_error));
    return;
  }
  printf("Rebooting...\n");
  sleep_ms(100);
  watchdog_reboot(0, 0, 0);
  for (;;)
    tight_loop_contents();
}

int main(void) {
  const uint32_t target_clock_khz = 144000u;
#if defined(PICO_RP2040) && PICO_RP2040
  vreg_set_voltage(VREG_VOLTAGE_1_20);
  sleep_ms(2);
#elif defined(PICO_RP2350) && PICO_RP2350
#else
#error unsupported Raspberry Pi silicon
#endif
  bool clock_ready = set_sys_clock_khz(target_clock_khz, true);
  stdio_init_all();
  sleep_ms(2000);

  printf("\r\n\r\n=== Pico Floppy Shell ===\r\n");
  if (!clock_ready || clock_get_hz(clk_sys) != target_clock_khz * 1000u) {
    printf(
        "Clock initialization failed: requested %lu kHz, running %lu kHz\r\n",
        (unsigned long)target_clock_khz,
        (unsigned long)(clock_get_hz(clk_sys) / 1000u));
    for (;;)
      tight_loop_contents();
  }
  cmd_version(0, NULL);

  block_status_t drive_error = floppy_init(&floppy, drive_pins);
  if (drive_error != BLOCK_OK) {
    printf("Drive initialization failed: %s\r\n",
           block_status_text(drive_error));
    for (;;)
      tight_loop_contents();
  }

  f12_err_t init_error = f12_init(&fs, floppy_device(&floppy));
  if (init_error != F12_OK) {
    printf("Filesystem initialization failed: %s\r\n",
           f12_strerror(init_error));
    block_status_t cleanup_error = floppy_deinit(&floppy);
    if (cleanup_error != BLOCK_OK) {
      printf("Drive cleanup failed: %s\r\n", block_status_text(cleanup_error));
    }
    for (;;)
      tight_loop_contents();
  }

  printf("Drive initialized (HD mode)\r\n");
  printf("Type 'help' for commands, 'mount' when disk is ready.\r\n\r\n");

  char *argv[MAX_ARGS];

  for (;;) {
    print_prompt();
    int len = cli_readline(cmd_buf);
    if (len == CLI_INPUT_CANCELLED)
      continue;
    if (len == CLI_INPUT_OVERFLOW) {
      printf("Command line too long.\n");
      continue;
    }
    if (len == 0)
      continue;

    int argc = tokenize(cmd_buf, argv, MAX_ARGS);
    if (argc < 0) {
      printf("Too many arguments.\n");
      continue;
    }
    if (argc == 0)
      continue;

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
      f12_err_t error = f12_is_mounted(&fs, &mounted);
      if (error != F12_OK) {
        printf("Filesystem state check failed: %s\n", f12_strerror(error));
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
