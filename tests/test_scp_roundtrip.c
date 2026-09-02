#include "test.h"
#include "flux_sim.h"
#include "scp_disk.h"
#include "scp_fixture.h"
#include "../src/f12.h"
#include "../src/mfm.h"
#include <errno.h>
#include <limits.h>

typedef struct {
  const char *fixture;
  uint32_t seed;
  uint32_t iterations;
} options_t;

typedef struct {
  uint8_t (*sectors)[DISK_SECTOR_SIZE];
  uint32_t generation;
  bool write_protected;
} image_disk_t;

typedef struct {
  char name[13];
  uint32_t size;
  uint32_t pattern;
} manifest_t;

static options_t options = {
    .seed = 0xC0DEC0DEu,
    .iterations = 4u,
};
static uint32_t initial_seed;
static uint32_t random_state;
static uint8_t *fixture_data;
static size_t fixture_size;
static scp_disk_t fixture_disk;
static uint8_t original[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE];
static uint8_t modified[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE];
static uint8_t decoded[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE];
static bool coverage[DISK_SECTOR_COUNT];

static bool parse_u32(const char *text, uint32_t *value) {
  if (!text || !*text || !value || text[0] == '-') return false;
  errno = 0;
  char *end;
  unsigned long parsed = strtoul(text, &end, 0);
  if (errno != 0 || *end != '\0' || parsed > UINT32_MAX) return false;
  *value = (uint32_t)parsed;
  return true;
}

static bool parse_options(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) {
      options.fixture = argv[++i];
    } else if ((strcmp(argv[i], "--seed") == 0 ||
                strcmp(argv[i], "-s") == 0) && i + 1 < argc) {
      if (!parse_u32(argv[++i], &options.seed)) return false;
    } else if ((strcmp(argv[i], "--iterations") == 0 ||
                strcmp(argv[i], "-n") == 0) && i + 1 < argc) {
      if (!parse_u32(argv[++i], &options.iterations) ||
          options.iterations == 0) {
        return false;
      }
    } else {
      return false;
    }
  }
  return options.fixture != NULL;
}

static uint32_t random_next(void) {
  random_state = random_state * 1664525u + 1013904223u;
  return random_state;
}

static uint8_t pattern_byte(uint32_t pattern, uint32_t offset) {
  uint32_t value = pattern ^ (offset * 0x9E3779B9u);
  value ^= value >> 16;
  value *= 0x7FEB352Du;
  value ^= value >> 15;
  return (uint8_t)value;
}

static uint32_t checksum(const uint8_t *data, size_t size) {
  uint32_t value = 2166136261u;
  for (size_t i = 0; i < size; i++) value = (value ^ data[i]) * 16777619u;
  return value;
}

static bool decode_scp(uint8_t *data, size_t size,
                       uint8_t out[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE]) {
  memset(out, 0, DISK_SECTOR_COUNT * DISK_SECTOR_SIZE);
  memset(coverage, 0, sizeof(coverage));
  flux_sim_t sim;
  if (!flux_sim_open_scp(&sim, data, size)) return false;

  for (uint8_t cylinder = 0; cylinder < DISK_CYLINDERS; cylinder++) {
    for (uint8_t head = 0; head < DISK_HEADS; head++) {
      track_t track = {.cylinder = cylinder, .head = head};
      for (uint8_t revolution = 0;
           revolution < sim.num_revolutions &&
           track.valid != DISK_TRACK_VALID;
           revolution++) {
        if (!flux_sim_seek(&sim, cylinder, head, revolution)) continue;
        mfm_t decoder;
        mfm_init(&decoder);
        mfm_sector_t sector;
        uint16_t delta;
        while (flux_sim_next(&sim, &delta)) {
          if (!mfm_feed(&decoder, delta, &sector)) continue;
          if (sector.cylinder != cylinder || sector.head != head ||
              !disk_sector_valid(sector.sector) ||
              track_has(&track, sector.sector)) {
            continue;
          }
          memcpy(track.data[sector.sector], sector.data, DISK_SECTOR_SIZE);
          track_mark(&track, sector.sector);
        }
      }
      if (track.valid != DISK_TRACK_VALID) {
        flux_sim_close(&sim);
        return false;
      }
      for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
        uint16_t lba;
        if (!disk_chs_to_lba(cylinder, head, sector, &lba)) {
          flux_sim_close(&sim);
          return false;
        }
        memcpy(out[lba], track.data[sector], DISK_SECTOR_SIZE);
        coverage[lba] = true;
      }
    }
  }
  flux_sim_close(&sim);
  for (uint16_t lba = 0; lba < DISK_SECTOR_COUNT; lba++) {
    if (!coverage[lba]) return false;
  }
  return true;
}

static disk_err_t image_read_track(void *ctx, uint32_t expected_generation,
                                       uint8_t cylinder, uint8_t head,
                                       track_t *track) {
  image_disk_t *disk = (image_disk_t *)ctx;
  if (!disk || !track || !disk_ch_valid(cylinder, head)) {
    return DISK_ERR_INVALID;
  }
  if (expected_generation != disk->generation) return DISK_ERR_MEDIA_CHANGED;
  memset(track, 0, sizeof(*track));
  track->cylinder = cylinder;
  track->head = head;
  track->valid = DISK_TRACK_VALID;
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    uint16_t lba;
    if (!disk_chs_to_lba(cylinder, head, sector, &lba)) {
      return DISK_ERR_INVALID;
    }
    memcpy(track->data[sector], disk->sectors[lba], DISK_SECTOR_SIZE);
  }
  return DISK_OK;
}

static disk_err_t image_write_track(void *ctx,
                                        uint32_t expected_generation,
                                        const track_t *track) {
  image_disk_t *disk = (image_disk_t *)ctx;
  if (!disk || !track ||
      !disk_ch_valid(track->cylinder, track->head) ||
      track->valid != DISK_TRACK_VALID) {
    return DISK_ERR_INVALID;
  }
  if (expected_generation != disk->generation) return DISK_ERR_MEDIA_CHANGED;
  if (disk->write_protected) return DISK_ERR_WRITE_PROTECTED;
  for (uint8_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    uint16_t lba;
    if (!disk_chs_to_lba(track->cylinder, track->head, sector, &lba)) {
      return DISK_ERR_INVALID;
    }
    memcpy(disk->sectors[lba], track->data[sector], DISK_SECTOR_SIZE);
  }
  return DISK_OK;
}

static disk_err_t image_generation(void *ctx, uint32_t *generation) {
  image_disk_t *disk = ctx;
  if (!disk || !generation) return DISK_ERR_INVALID;
  *generation = disk->generation;
  return DISK_OK;
}

static disk_err_t image_write_protected(void *ctx, bool *write_protected) {
  image_disk_t *disk = ctx;
  if (!disk || !write_protected) return DISK_ERR_INVALID;
  *write_protected = disk->write_protected;
  return DISK_OK;
}

static disk_device_t image_device(image_disk_t *disk) {
  return (disk_device_t){
      .read_track = image_read_track,
      .write_track = image_write_track,
      .media_generation = image_generation,
      .write_protected = image_write_protected,
      .ctx = disk,
  };
}

static disk_result_t write_all(f12_file_t *file, const uint8_t *data,
                              size_t size) {
  disk_result_t total = {.error = DISK_OK, .count = 0};
  while (total.count < size) {
    disk_result_t part = f12_write(file, data + total.count,
                                  size - total.count);
    total.count += part.count;
    if (part.error != DISK_OK) {
      total.error = part.error;
      return total;
    }
    if (part.count == 0) {
      total.error = DISK_ERR_IO;
      return total;
    }
  }
  return total;
}

static disk_result_t read_all(f12_file_t *file, uint8_t *data, size_t size) {
  disk_result_t total = {.error = DISK_OK, .count = 0};
  while (total.count < size) {
    disk_result_t part = f12_read(file, data + total.count, size - total.count);
    total.count += part.count;
    if (part.error != DISK_OK) {
      total.error = part.error;
      return total;
    }
    if (part.count == 0) {
      total.error = DISK_ERR_IO;
      return total;
    }
  }
  return total;
}

static bool first_regular_file(f12_t *fs, char name[13]) {
  f12_dir_t dir;
  if (f12_opendir(fs, "/", &dir) != DISK_OK) return false;
  bool found = false;
  for (;;) {
    f12_stat_t stat;
    disk_err_t error = f12_readdir(&dir, &stat);
    if (error == DISK_END) break;
    if (error != DISK_OK) {
      f12_closedir(&dir);
      return false;
    }
    if ((stat.attr & (FAT12_ATTR_DIRECTORY | FAT12_ATTR_VOLUME_ID)) == 0) {
      memcpy(name, stat.name, sizeof(stat.name));
      found = true;
      break;
    }
  }
  if (f12_closedir(&dir) != DISK_OK) return false;
  return found;
}

static void assert_images_equal(
    const uint8_t left[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE],
    const uint8_t right[DISK_SECTOR_COUNT][DISK_SECTOR_SIZE]) {
  for (uint16_t lba = 0; lba < DISK_SECTOR_COUNT; lba++) {
    ASSERT_MEM_EQ(left[lba], right[lba], DISK_SECTOR_SIZE);
  }
}

TEST(test_fixture_decode_uses_supplied_path) {
  ASSERT(decode_scp(fixture_data, fixture_size, original));
  uint32_t value = 0;
  for (uint16_t lba = 0; lba < DISK_SECTOR_COUNT; lba++) {
    ASSERT(coverage[lba]);
    value ^= checksum(original[lba], DISK_SECTOR_SIZE);
  }
  ASSERT(value != 0);
}

TEST(test_fixture_flux_roundtrip) {
  size_t encoded_size;
  uint8_t *encoded = scp_encode_disk(original, &encoded_size);
  ASSERT(encoded != NULL);
  ASSERT(encoded_size > 0);
  ASSERT(decode_scp(encoded, encoded_size, decoded));
  assert_images_equal(original, decoded);
  free(encoded);
}

TEST(test_f12_mutation_survives_flux_roundtrip) {
  memcpy(modified, original, sizeof(modified));
  image_disk_t disk = {
      .sectors = modified,
      .generation = 1,
  };
  f12_t fs;
  ASSERT_EQ(f12_init(&fs, image_device(&disk)), DISK_OK);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  char old_name[13] = {0};
  ASSERT(first_regular_file(&fs, old_name));
  char new_name[13] = {0};
  bool available = false;
  for (unsigned candidate = 0; candidate < 100; candidate++) {
    snprintf(new_name, sizeof(new_name), "RT%02u.BIN", candidate);
    f12_stat_t stat;
    disk_err_t error = f12_stat(&fs, new_name, &stat);
    if (error == DISK_ERR_NOT_FOUND) {
      available = true;
      break;
    }
    ASSERT_EQ(error, DISK_OK);
  }
  ASSERT(available);
  ASSERT_EQ(f12_rename(&fs, old_name, new_name), DISK_OK);
  f12_stat_t expected;
  ASSERT_EQ(f12_stat(&fs, new_name, &expected), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);

  size_t encoded_size;
  uint8_t *encoded = scp_encode_disk(modified, &encoded_size);
  ASSERT(encoded != NULL);
  ASSERT(decode_scp(encoded, encoded_size, decoded));
  assert_images_equal(modified, decoded);
  free(encoded);

  image_disk_t roundtrip = {
      .sectors = decoded,
      .generation = 1,
  };
  ASSERT_EQ(f12_init(&fs, image_device(&roundtrip)), DISK_OK);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  f12_stat_t actual;
  ASSERT_EQ(f12_stat(&fs, old_name, &actual), DISK_ERR_NOT_FOUND);
  ASSERT_EQ(f12_stat(&fs, new_name, &actual), DISK_OK);
  ASSERT_EQ(actual.size, expected.size);
  ASSERT_EQ(actual.attr, expected.attr);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_deterministic_fuzz_roundtrips) {
  uint8_t buffer[4096];
  uint8_t verify[4096];
  uint32_t completed = 0;
  for (uint32_t iteration = 0; iteration < options.iterations; iteration++) {
    memset(modified, 0, sizeof(modified));
    image_disk_t disk = {
        .sectors = modified,
        .generation = 1,
    };
    f12_t fs;
    ASSERT_EQ(f12_init(&fs, image_device(&disk)), DISK_OK);
    f12_format_options_t format = {
        .label = "ROUNDTRIP",
        .mode = F12_FORMAT_QUICK,
    };
    ASSERT_EQ(f12_format(&fs, format), DISK_OK);
    ASSERT_EQ(f12_mount(&fs), DISK_OK);

    manifest_t manifest[8];
    size_t manifest_count = 2u + random_next() % 7u;
    for (size_t index = 0; index < manifest_count; index++) {
      manifest_t *entry = &manifest[index];
      snprintf(entry->name, sizeof(entry->name), "I%03uF%02u.BIN",
               (unsigned)(iteration % 1000u), (unsigned)(index % 100u));
      entry->size = 1u + random_next() % sizeof(buffer);
      entry->pattern = random_next();
      for (uint32_t offset = 0; offset < entry->size; offset++) {
        buffer[offset] = pattern_byte(entry->pattern, offset);
      }
      f12_file_t file;
      ASSERT_EQ(f12_open(&fs, entry->name, F12_OPEN_WRITE, &file), DISK_OK);
      disk_result_t result = write_all(&file, buffer, entry->size);
      ASSERT_EQ(result.error, DISK_OK);
      ASSERT_EQ(result.count, entry->size);
      ASSERT_EQ(f12_close(&file), DISK_OK);
    }
    ASSERT_EQ(f12_unmount(&fs), DISK_OK);

    size_t encoded_size;
    uint8_t *encoded = scp_encode_disk(modified, &encoded_size);
    ASSERT(encoded != NULL);
    ASSERT(decode_scp(encoded, encoded_size, decoded));
    assert_images_equal(modified, decoded);
    free(encoded);

    image_disk_t roundtrip = {
        .sectors = decoded,
        .generation = 1,
    };
    ASSERT_EQ(f12_init(&fs, image_device(&roundtrip)), DISK_OK);
    ASSERT_EQ(f12_mount(&fs), DISK_OK);
    for (size_t index = 0; index < manifest_count; index++) {
      manifest_t *entry = &manifest[index];
      f12_stat_t stat;
      ASSERT_EQ(f12_stat(&fs, entry->name, &stat), DISK_OK);
      ASSERT_EQ(stat.size, entry->size);
      f12_file_t file;
      ASSERT_EQ(f12_open(&fs, entry->name, F12_OPEN_READ, &file), DISK_OK);
      disk_result_t result = read_all(&file, verify, entry->size);
      ASSERT_EQ(result.error, DISK_OK);
      ASSERT_EQ(result.count, entry->size);
      ASSERT_EQ(f12_close(&file), DISK_OK);
      for (uint32_t offset = 0; offset < entry->size; offset++) {
        ASSERT_EQ(verify[offset], pattern_byte(entry->pattern, offset));
      }
    }
    ASSERT_EQ(f12_unmount(&fs), DISK_OK);
    completed++;
    printf("  iteration %u/%u complete\n", completed, options.iterations);
  }
  ASSERT_EQ(completed, options.iterations);
  printf("  reproducible seed: %u\n", initial_seed);
}

int main(int argc, char **argv) {
  if (!parse_options(argc, argv)) {
    fprintf(stderr,
            "Usage: %s --fixture PATH [--seed N] [--iterations N]\n",
            argv[0]);
    return 2;
  }
  fixture_data = scp_fixture_load(options.fixture, &fixture_size);
  if (!fixture_data) {
    fprintf(stderr, "Cannot read required SCP fixture %s: %s\n",
            options.fixture, strerror(errno));
    return 1;
  }
  if (!scp_disk_init(&fixture_disk, fixture_data, fixture_size)) {
    fprintf(stderr, "Invalid SCP fixture: %s\n", options.fixture);
    free(fixture_data);
    return 1;
  }

  initial_seed = options.seed;
  random_state = initial_seed;
  printf("=== SCP Roundtrip Contract Tests ===\n");
  printf("Fixture: %s (%zu bytes)\n", options.fixture, fixture_size);
  printf("Seed: %u\n", initial_seed);
  printf("Iterations: %u\n\n", options.iterations);
  RUN_TEST(test_fixture_decode_uses_supplied_path);
  RUN_TEST(test_fixture_flux_roundtrip);
  RUN_TEST(test_f12_mutation_survives_flux_roundtrip);
  RUN_TEST(test_deterministic_fuzz_roundtrips);
  scp_disk_deinit(&fixture_disk);
  free(fixture_data);
  TEST_RESULTS();
}
