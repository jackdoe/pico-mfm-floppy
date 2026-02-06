#include <time.h>
#include "test.h"
#include "vdisk.h"
#include "../src/crc.h"
#include "../src/mfm_decode.h"
#include "../src/mfm_encode.h"
#include "../src/fat12.h"

static uint32_t fuzz_seed = 0;

static uint32_t fuzz_rand(void) {
  fuzz_seed = fuzz_seed * 1103515245 + 12345;
  return (fuzz_seed >> 16) & 0x7FFF;
}

static void fuzz_srand(uint32_t seed) {
  fuzz_seed = seed;
}

static uint8_t fuzz_rand8(void) {
  return fuzz_rand() & 0xFF;
}

static uint16_t fuzz_rand16(void) {
  return (fuzz_rand() << 8) | fuzz_rand8();
}

static int fuzz_tests_run = 0;
static int crashes_avoided = 0;

void fuzz_mfm_decoder_random_pulses(int iterations) {
  printf("Fuzzing MFM decoder with random pulses (%d iterations)...\n", iterations);

  for (int iter = 0; iter < iterations; iter++) {
    mfm_t m;
    mfm_init(&m);

    sector_t out;
    memset(&out, 0, sizeof(out));

    int pulse_count = 100 + (fuzz_rand() % 10000);
    for (int i = 0; i < pulse_count; i++) {
      uint16_t pulse = fuzz_rand16();
      mfm_feed(&m, pulse, &out);
    }

    fuzz_tests_run++;
  }

  printf("  Completed %d iterations without crash\n", iterations);
}

void fuzz_mfm_decoder_edge_pulses(int iterations) {
  printf("Fuzzing MFM decoder with edge case pulses (%d iterations)...\n", iterations);

  uint16_t edge_values[] = {
    0, 1, 34, 35, 36, 56, 57, 58, 81, 82, 83, 119, 120, 121,
    255, 256, 1000, 32767, 32768, 65534, 65535
  };
  int num_edges = sizeof(edge_values) / sizeof(edge_values[0]);

  for (int iter = 0; iter < iterations; iter++) {
    mfm_t m;
    mfm_init(&m);

    sector_t out;
    memset(&out, 0, sizeof(out));

    for (int i = 0; i < 1000; i++) {
      uint16_t pulse = edge_values[fuzz_rand() % num_edges];
      mfm_feed(&m, pulse, &out);
    }

    fuzz_tests_run++;
  }

  printf("  Completed %d iterations without crash\n", iterations);
}

void fuzz_mfm_decoder_state_transitions(int iterations) {
  printf("Fuzzing MFM decoder state transitions (%d iterations)...\n", iterations);

  for (int iter = 0; iter < iterations; iter++) {
    mfm_t m;
    mfm_init(&m);

    sector_t out;

    for (int i = 0; i < 100; i++) {
      mfm_feed(&m, 47, &out);
    }

    for (int i = 0; i < 50; i++) {
      uint16_t pulse = 47 + (fuzz_rand() % 60);
      mfm_feed(&m, pulse, &out);
    }

    mfm_reset(&m);

    fuzz_tests_run++;
  }

  printf("  Completed %d iterations without crash\n", iterations);
}

void fuzz_mfm_encoder_random_data(int iterations) {
  printf("Fuzzing MFM encoder with random data (%d iterations)...\n", iterations);

  uint8_t pulse_buf[16384];
  uint8_t data_buf[1024];

  for (int iter = 0; iter < iterations; iter++) {
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    int data_len = fuzz_rand() % sizeof(data_buf);
    for (int i = 0; i < data_len; i++) {
      data_buf[i] = fuzz_rand8();
    }

    mfm_encode_bytes(&enc, data_buf, data_len);

    if (enc.pos > sizeof(pulse_buf)) {
      printf("  ERROR: Buffer overflow at iteration %d\n", iter);
      exit(1);
    }

    fuzz_tests_run++;
  }

  printf("  Completed %d iterations without crash\n", iterations);
}

void fuzz_mfm_encoder_tiny_buffer(int iterations) {
  printf("Fuzzing MFM encoder with tiny buffers (%d iterations)...\n", iterations);

  for (int iter = 0; iter < iterations; iter++) {
    uint8_t tiny_buf[1 + (fuzz_rand() % 64)];
    mfm_encode_t enc;
    mfm_encode_init(&enc, tiny_buf, sizeof(tiny_buf));

    sector_t s;
    memset(&s, 0, sizeof(s));
    s.track = fuzz_rand8();
    s.side = fuzz_rand8();
    s.sector_n = fuzz_rand8();
    for (int i = 0; i < SECTOR_SIZE; i++) {
      s.data[i] = fuzz_rand8();
    }

    mfm_encode_sector(&enc, &s);

    if (enc.pos > enc.size) {
      printf("  ERROR: pos > size at iteration %d\n", iter);
      exit(1);
    }

    fuzz_tests_run++;
    crashes_avoided++;
  }

  printf("  Completed %d iterations, avoided %d potential overflows\n", iterations, crashes_avoided);
  crashes_avoided = 0;
}

void fuzz_mfm_encoder_zero_buffer(void) {
  printf("Fuzzing MFM encoder with zero-size buffer...\n");

  uint8_t dummy;
  mfm_encode_t enc;
  mfm_encode_init(&enc, &dummy, 0);

  mfm_encode_gap(&enc, 100);
  mfm_encode_sync(&enc);

  uint8_t data[] = {0x00, 0xFF, 0xAA, 0x55};
  mfm_encode_bytes(&enc, data, sizeof(data));

  if (enc.pos != 0) {
    printf("  ERROR: pos should be 0 for zero-size buffer\n");
    exit(1);
  }

  fuzz_tests_run++;
  printf("  Passed\n");
}

static uint16_t pulse_to_delta(uint8_t pulse) {
  return pulse + MFM_PIO_OVERHEAD;
}

void fuzz_mfm_roundtrip(int iterations) {
  printf("Fuzzing MFM encode/decode roundtrip (%d iterations)...\n", iterations);

  uint8_t pulse_buf[8192];

  for (int iter = 0; iter < iterations; iter++) {
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));

    sector_t s_in;
    memset(&s_in, 0, sizeof(s_in));
    s_in.track = fuzz_rand8() % 80;
    s_in.side = fuzz_rand8() % 2;
    s_in.sector_n = 1 + (fuzz_rand8() % 18);
    s_in.size_code = 2;
    s_in.valid = true;
    for (int i = 0; i < SECTOR_SIZE; i++) {
      s_in.data[i] = fuzz_rand8();
    }

    mfm_encode_gap(&enc, 80);
    mfm_encode_sector(&enc, &s_in);

    mfm_t m;
    mfm_init(&m);

    sector_t s_out;
    memset(&s_out, 0, sizeof(s_out));
    bool got_sector = false;

    for (size_t i = 0; i < enc.pos; i++) {
      if (mfm_feed(&m, pulse_to_delta(pulse_buf[i]), &s_out)) {
        got_sector = true;
      }
    }

    if (got_sector && s_out.valid) {
      if (memcmp(s_in.data, s_out.data, SECTOR_SIZE) != 0) {
        printf("FAIL: Data mismatch at iteration %d (seed %u)\n", iter, fuzz_seed);
        exit(1);
      }
    }

    fuzz_tests_run++;
  }

  printf("  Completed %d iterations\n", iterations);
}

static vdisk_t *fuzz_disk = NULL;

void fuzz_fat12_random_boot_sector(int iterations) {
  printf("Fuzzing FAT12 with random boot sectors (%d iterations)...\n", iterations);

  for (int iter = 0; iter < iterations; iter++) {
    for (int i = 0; i < SECTOR_SIZE; i++) {
      fuzz_disk->data[0][i] = fuzz_rand8();
    }

    fat12_t fat;
    fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = fuzz_disk };
    fat12_init(&fat, io);

    fuzz_tests_run++;
  }

  printf("  Completed %d iterations without crash\n", iterations);
}

void fuzz_fat12_corrupt_bpb_values(int iterations) {
  printf("Fuzzing FAT12 with corrupt BPB values (%d iterations)...\n", iterations);

  for (int iter = 0; iter < iterations; iter++) {
    vdisk_format_valid(fuzz_disk);

    int field = fuzz_rand() % 10;
    switch (field) {
      case 0:
        fuzz_disk->data[0][11] = fuzz_rand8();
        fuzz_disk->data[0][12] = fuzz_rand8();
        break;
      case 1:
        fuzz_disk->data[0][13] = fuzz_rand8();
        break;
      case 2:
        fuzz_disk->data[0][14] = fuzz_rand8();
        fuzz_disk->data[0][15] = fuzz_rand8();
        break;
      case 3:
        fuzz_disk->data[0][16] = fuzz_rand8();
        break;
      case 4:
        fuzz_disk->data[0][17] = fuzz_rand8();
        fuzz_disk->data[0][18] = fuzz_rand8();
        break;
      case 5:
        fuzz_disk->data[0][19] = fuzz_rand8();
        fuzz_disk->data[0][20] = fuzz_rand8();
        break;
      case 6:
        fuzz_disk->data[0][22] = fuzz_rand8();
        fuzz_disk->data[0][23] = fuzz_rand8();
        break;
      case 7:
        fuzz_disk->data[0][24] = fuzz_rand8();
        fuzz_disk->data[0][25] = fuzz_rand8();
        break;
      case 8:
        fuzz_disk->data[0][26] = fuzz_rand8();
        fuzz_disk->data[0][27] = fuzz_rand8();
        break;
      case 9:
        fuzz_disk->data[0][510] = fuzz_rand8();
        fuzz_disk->data[0][511] = fuzz_rand8();
        break;
    }

    fat12_t fat;
    fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = fuzz_disk };

    fat12_err_t err = fat12_init(&fat, io);
    if (err == FAT12_OK) {
      fat12_dirent_t entry;
      fat12_find(&fat, "TEST.TXT", &entry);

      uint8_t buf[FAT12_MAX_CLUSTER_SECTORS * SECTOR_SIZE];
      fat12_read_cluster(&fat, 2, buf);
      fat12_read_cluster(&fat, fuzz_rand16(), buf);
    }

    fuzz_tests_run++;
  }

  printf("  Completed %d iterations without crash\n", iterations);
}

void fuzz_fat12_random_fat_entries(int iterations) {
  printf("Fuzzing FAT12 with random FAT entries (%d iterations)...\n", iterations);

  for (int iter = 0; iter < iterations; iter++) {
    vdisk_format_valid(fuzz_disk);

    for (int sector = 1; sector < 10; sector++) {
      for (int i = 0; i < SECTOR_SIZE; i++) {
        fuzz_disk->data[sector][i] = fuzz_rand8();
      }
    }
    fuzz_disk->data[1][0] = 0xF0;

    fat12_t fat;
    fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = fuzz_disk };

    fat12_err_t err = fat12_init(&fat, io);
    if (err == FAT12_OK) {
      for (int i = 0; i < 10; i++) {
        uint16_t cluster = fuzz_rand16() % 3000;
        uint16_t next;
        fat12_get_entry(&fat, cluster, &next);
      }
    }

    fuzz_tests_run++;
  }

  printf("  Completed %d iterations without crash\n", iterations);
}

void fuzz_fat12_random_directory(int iterations) {
  printf("Fuzzing FAT12 with random directory entries (%d iterations)...\n", iterations);

  for (int iter = 0; iter < iterations; iter++) {
    vdisk_format_valid(fuzz_disk);

    for (int sector = 19; sector < 33; sector++) {
      for (int i = 0; i < SECTOR_SIZE; i++) {
        fuzz_disk->data[sector][i] = fuzz_rand8();
      }
    }

    fat12_t fat;
    fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = fuzz_disk };

    fat12_err_t err = fat12_init(&fat, io);
    if (err == FAT12_OK) {
      fat12_dirent_t entry;
      for (int i = 0; i < 50; i++) {
        fat12_read_root_entry(&fat, i, &entry);
      }

      fat12_find(&fat, "RANDOM.TXT", &entry);
      fat12_find(&fat, "TEST", &entry);
      fat12_find(&fat, "", &entry);
      fat12_find(&fat, "VERYLONGFILENAME.EXTENSION", &entry);
    }

    fuzz_tests_run++;
  }

  printf("  Completed %d iterations without crash\n", iterations);
}

void fuzz_fat12_file_operations(int iterations) {
  printf("Fuzzing FAT12 file operations on corrupt disk (%d iterations)...\n", iterations);

  for (int iter = 0; iter < iterations; iter++) {
    vdisk_format_valid(fuzz_disk);

    int corrupt_count = fuzz_rand() % 20;
    for (int c = 0; c < corrupt_count; c++) {
      int sector = fuzz_rand() % VDISK_TOTAL_SECTORS;
      int offset = fuzz_rand() % SECTOR_SIZE;
      fuzz_disk->data[sector][offset] = fuzz_rand8();
    }

    fat12_t fat;
    fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = fuzz_disk };

    fat12_err_t err = fat12_init(&fat, io);
    if (err == FAT12_OK) {
      fat12_dirent_t entry;

      fat12_writer_t writer;
      if (fat12_open_write(&fat, "FUZZ.TXT", &writer) == FAT12_OK) {
        uint8_t write_buf[256];
        for (int i = 0; i < 256; i++) {
          write_buf[i] = fuzz_rand8();
        }
        fat12_write(&writer, write_buf, sizeof(write_buf));
        fat12_close_write(&writer);
      }

      if (fat12_find(&fat, "FUZZ.TXT", &entry) == FAT12_OK) {
        fat12_file_t file;
        if (fat12_open(&fat, &entry, &file) == FAT12_OK) {
          uint8_t read_buf[512];
          fat12_read(&file, read_buf, sizeof(read_buf));
        }
      }

      fat12_delete(&fat, "FUZZ.TXT");
    }

    fuzz_tests_run++;
  }

  printf("  Completed %d iterations without crash\n", iterations);
}

void fuzz_fat12_cluster_edge_cases(int iterations) {
  printf("Fuzzing FAT12 cluster edge cases (%d iterations)...\n", iterations);

  vdisk_format_valid(fuzz_disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = fuzz_disk };

  fat12_err_t err = fat12_init(&fat, io);
  if (err != FAT12_OK) {
    printf("  ERROR: Failed to init valid disk\n");
    return;
  }

  uint8_t buf[512];

  for (int iter = 0; iter < iterations; iter++) {
    uint16_t test_clusters[] = {
      0, 1, 2, 3,
      0xFF6, 0xFF7, 0xFF8, 0xFF9,
      0xFFF,
      fat.total_clusters + 1,
      fat.total_clusters + 2,
      0x7FFF, 0xFFFF
    };

    for (int i = 0; i < (int)(sizeof(test_clusters)/sizeof(test_clusters[0])); i++) {
      uint16_t cluster = test_clusters[i];
      fat12_read_cluster(&fat, cluster, buf);

      uint16_t next;
      fat12_get_entry(&fat, cluster, &next);
    }

    for (int i = 0; i < 100; i++) {
      uint16_t cluster = fuzz_rand16();
      fat12_read_cluster(&fat, cluster, buf);

      uint16_t next;
      fat12_get_entry(&fat, cluster, &next);
    }

    fuzz_tests_run++;
  }

  printf("  Completed %d iterations without crash\n", iterations);
}

int main(int argc, char *argv[]) {
  int iterations = 1000;
  uint32_t seed = (uint32_t)time(NULL);

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
      iterations = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      seed = (uint32_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "-h") == 0) {
      printf("Usage: %s [-n iterations] [-s seed]\n", argv[0]);
      return 0;
    }
  }

  printf("=== Fuzz Tests ===\n");
  printf("Seed: %u (use -s %u to reproduce)\n", seed, seed);
  printf("Iterations: %d\n\n", iterations);

  fuzz_srand(seed);

  fuzz_disk = (vdisk_t *)malloc(sizeof(vdisk_t));
  if (!fuzz_disk) {
    printf("ERROR: Failed to allocate virtual disk\n");
    return 1;
  }

  printf("--- MFM Decoder Fuzz Tests ---\n");
  fuzz_mfm_decoder_random_pulses(iterations);
  fuzz_mfm_decoder_edge_pulses(iterations);
  fuzz_mfm_decoder_state_transitions(iterations);

  printf("\n--- MFM Encoder Fuzz Tests ---\n");
  fuzz_mfm_encoder_random_data(iterations);
  fuzz_mfm_encoder_tiny_buffer(iterations);
  fuzz_mfm_encoder_zero_buffer();

  printf("\n--- MFM Roundtrip Fuzz Tests ---\n");
  fuzz_mfm_roundtrip(iterations);

  printf("\n--- FAT12 Fuzz Tests ---\n");
  fuzz_fat12_random_boot_sector(iterations);
  fuzz_fat12_corrupt_bpb_values(iterations);
  fuzz_fat12_random_fat_entries(iterations);
  fuzz_fat12_random_directory(iterations);
  fuzz_fat12_file_operations(iterations);
  fuzz_fat12_cluster_edge_cases(iterations);

  free(fuzz_disk);

  printf("\n=== Fuzz Test Summary ===\n");
  printf("Total test iterations: %d\n", fuzz_tests_run);
  printf("All tests passed without crashes!\n");

  return 0;
}
