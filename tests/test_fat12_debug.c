#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../src/floppy.h"
#include "../src/fat12.h"

// Virtual disk for testing
#define VDISK_TRACKS 80
#define VDISK_SIDES 2
#define VDISK_TOTAL_SECTORS (VDISK_TRACKS * VDISK_SIDES * SECTORS_PER_TRACK)

typedef struct {
  uint8_t data[VDISK_TOTAL_SECTORS][SECTOR_SIZE];
} vdisk_t;

static inline int vdisk_lba(uint8_t track, uint8_t side, uint8_t sector_n) {
  return (track * VDISK_SIDES + side) * SECTORS_PER_TRACK + (sector_n - 1);
}

bool vdisk_read(void *ctx, sector_t *sector) {
  vdisk_t *disk = (vdisk_t *)ctx;
  int lba = vdisk_lba(sector->track, sector->side, sector->sector_n);
  if (lba < 0 || lba >= VDISK_TOTAL_SECTORS) {
    sector->valid = false;
    return false;
  }
  memcpy(sector->data, disk->data[lba], SECTOR_SIZE);
  sector->valid = true;
  sector->size = SECTOR_SIZE;
  sector->size_code = 2;
  return true;
}

bool vdisk_write(void *ctx, track_t *track) {
  vdisk_t *disk = (vdisk_t *)ctx;

  printf("WRITE TRACK C%d H%d:\n", track->track, track->side);

  // Complete missing sectors
  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    if (!track->sectors[i].valid) {
      int lba = vdisk_lba(track->track, track->side, i + 1);
      if (lba >= 0 && lba < VDISK_TOTAL_SECTORS) {
        memcpy(track->sectors[i].data, disk->data[lba], SECTOR_SIZE);
        track->sectors[i].valid = true;
      }
    }
  }

  // Debug: check sector 16 (index 15) when writing track 0, side 1
  if (track->track == 0 && track->side == 1) {
    printf("  Sector 16 (LBA 33) bytes 460-475 before write: ");
    for (int i = 460; i < 476; i++) {
      printf("%02X ", track->sectors[15].data[i]);
    }
    printf("\n");
  }

  // Write all sectors
  for (int i = 0; i < SECTORS_PER_TRACK; i++) {
    int lba = vdisk_lba(track->track, track->side, i + 1);
    if (lba >= 0 && lba < VDISK_TOTAL_SECTORS) {
      memcpy(disk->data[lba], track->sectors[i].data, SECTOR_SIZE);
    }
  }
  return true;
}

void vdisk_format(vdisk_t *disk) {
  memset(disk, 0, sizeof(*disk));
  uint8_t *boot = disk->data[0];
  boot[0] = 0xEB; boot[1] = 0x3C; boot[2] = 0x90;
  memcpy(&boot[3], "MSDOS5.0", 8);
  boot[11] = SECTOR_SIZE & 0xFF;
  boot[12] = SECTOR_SIZE >> 8;
  boot[13] = 1;  // sectors per cluster
  boot[14] = 1; boot[15] = 0;
  boot[16] = 2;
  boot[17] = 224; boot[18] = 0;
  boot[19] = (VDISK_TOTAL_SECTORS) & 0xFF;
  boot[20] = (VDISK_TOTAL_SECTORS) >> 8;
  boot[21] = 0xF0;
  boot[22] = 9; boot[23] = 0;
  boot[24] = 18; boot[25] = 0;
  boot[26] = 2; boot[27] = 0;
  boot[510] = 0x55;
  boot[511] = 0xAA;
  uint8_t *fat1 = disk->data[1];
  uint8_t *fat2 = disk->data[10];
  fat1[0] = 0xF0; fat1[1] = 0xFF; fat1[2] = 0xFF;
  fat2[0] = 0xF0; fat2[1] = 0xFF; fat2[2] = 0xFF;
}

// Debug: check batch contents after adding a sector
void debug_check_batch(fat12_write_batch_t *batch, uint16_t lba, const char *when) {
  printf("  Batch has %d entries (%s)\n", batch->count, when);
  for (int i = 0; i < batch->count; i++) {
    if (batch->lbas[i] == lba) {
      printf("  Batch entry %d for LBA %d: bytes 460-475: ", i, lba);
      for (int j = 460; j < 476; j++) {
        printf("%02X ", batch->data[i][j]);
      }
      printf("\n");
    }
  }
}

void debug_dump_batch(fat12_write_batch_t *batch) {
  printf("  Full batch dump (%d entries):\n", batch->count);
  for (int i = 0; i < batch->count; i++) {
    printf("    [%d] LBA %d, first 4 bytes: %02X %02X %02X %02X",
           i, batch->lbas[i],
           batch->data[i][0], batch->data[i][1],
           batch->data[i][2], batch->data[i][3]);
    if (batch->lbas[i] == 33) {
      printf(" <-- LBA 33! bytes 460-467: %02X %02X %02X %02X %02X %02X %02X %02X",
             batch->data[i][460], batch->data[i][461],
             batch->data[i][462], batch->data[i][463],
             batch->data[i][464], batch->data[i][465],
             batch->data[i][466], batch->data[i][467]);
    }
    printf("\n");
  }
}

int main(void) {
  printf("=== FAT12 Debug Test ===\n\n");

  vdisk_t disk;
  vdisk_format(&disk);

  fat12_t fat;
  fat12_io_t io = { .read = vdisk_read, .write = vdisk_write, .ctx = &disk };
  fat12_init(&fat, io);

  printf("Opening file for write...\n");
  fat12_writer_t writer;
  fat12_open_write(&fat, "BIG.DAT", &writer);

  // Create pattern: byte i = i & 0xFF
  uint8_t pattern[2000];
  for (int i = 0; i < 2000; i++) {
    pattern[i] = i & 0xFF;
  }

  printf("\nWriting 2000 bytes in chunks to trace...\n");

  // Write in smaller chunks to see what's happening
  int total = 0;
  int chunk_sizes[] = {512, 512, 512, 464};
  for (int c = 0; c < 4; c++) {
    printf("\n--- Chunk %d: writing %d bytes ---\n", c+1, chunk_sizes[c]);
    printf("Batch before write: %d entries\n", writer.batch.count);

    // Check what cluster find_free_cluster would return
    uint16_t next_free;
    fat12_find_free_cluster(&fat, &next_free);
    printf("Next free cluster (from disk): %d\n", next_free);

    int written = fat12_write(&writer, pattern + total, chunk_sizes[c]);
    total += written;

    printf("Batch after write: %d entries\n", writer.batch.count);
    printf("writer.current_cluster: %d\n", writer.current_cluster);
    debug_check_batch(&writer.batch, 33, "after chunk");
  }

  printf("\nTotal written: %d bytes\n", total);

  // Check batch contents before close
  printf("\nBatch contents before close:\n");
  debug_check_batch(&writer.batch, 33, "before close");
  debug_dump_batch(&writer.batch);

  printf("\nClosing file...\n");
  fat12_close_write(&writer);

  // Check final disk contents
  printf("\nFinal disk LBA 33 (cluster 2) bytes 460-475: ");
  for (int i = 460; i < 476; i++) {
    printf("%02X ", disk.data[33][i]);
  }
  printf("\n");

  printf("Expected:                                      ");
  for (int i = 460; i < 476; i++) {
    printf("%02X ", i & 0xFF);
  }
  printf("\n");

  // Check if data matches
  int mismatches = 0;
  int first_mismatch = -1;
  for (int i = 0; i < 512; i++) {
    if (disk.data[33][i] != (i & 0xFF)) {
      if (first_mismatch < 0) first_mismatch = i;
      mismatches++;
    }
  }

  if (mismatches > 0) {
    printf("\nFAILED: %d mismatches in cluster 2, first at byte %d\n", mismatches, first_mismatch);
    return 1;
  } else {
    printf("\nPASSED: Cluster 2 data correct\n");
    return 0;
  }
}
