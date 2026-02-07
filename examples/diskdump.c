#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "floppy.h"
#include "f12.h"

static uint32_t checksum_buf(const uint8_t *buf, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (sum << 5) + sum + buf[i];
    }
    return sum;
}

int main(void) {
    stdio_init_all();
    sleep_ms(3000);

    printf("\n\n========================================\n");
    printf("  FLOPPY DISK DUMP\n");
    printf("========================================\n\n");

    floppy_t floppy = {
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

    printf("[INIT] HD mode, write protect: %s\n\n",
           floppy_write_protected(&floppy) ? "YES" : "no");

    int total_valid = 0;
    int total_invalid = 0;
    uint32_t disk_checksum = 0;
    sector_t sector;

    printf("%-8s %-6s %-10s %-10s\n", "TRACK", "SIDE", "DECODED", "ERRORS");
    printf("%-8s %-6s %-10s %-10s\n", "-----", "----", "-------", "------");

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

            printf("T%02d      %d      %2d/%-2d      %d\n",
                   track, side, decoded, SECTORS_PER_TRACK, errors);
        }
    }

    printf("\n========================================\n");
    printf("  SECTOR SUMMARY\n");
    printf("========================================\n");
    printf("  Total decoded: %d / 2880\n", total_valid);
    printf("  Errors:        %d\n", total_invalid);
    printf("  Disk checksum: 0x%08lX\n", disk_checksum);

    printf("\n========================================\n");
    printf("  FAT12 INFO\n");
    printf("========================================\n");

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    f12_err_t err = f12_mount(&fs, (f12_io_t){
        .read = floppy_io_read,
        .write = floppy_io_write,
        .disk_changed = floppy_io_disk_changed,
        .write_protected = floppy_io_write_protected,
        .ctx = &floppy,
    });

    if (err == F12_OK) {
        fat12_bpb_t *bpb = &fs.fat.bpb;
        printf("  Bytes/sector:     %d\n", bpb->bytes_per_sector);
        printf("  Sectors/cluster:  %d\n", bpb->sectors_per_cluster);
        printf("  Reserved sectors: %d\n", bpb->reserved_sectors);
        printf("  FATs:             %d\n", bpb->num_fats);
        printf("  Root entries:     %d\n", bpb->root_entries);
        printf("  Total sectors:    %d\n", bpb->total_sectors);
        printf("  Media descriptor: 0x%02X\n", bpb->media_descriptor);
        printf("  Sectors/FAT:      %d\n", bpb->sectors_per_fat);
        printf("  Sectors/track:    %d\n", bpb->sectors_per_track);
        printf("  Heads:            %d\n", bpb->num_heads);

        printf("\n  %-12s %10s %10s\n", "NAME", "SIZE", "CHECKSUM");
        printf("  %-12s %10s %10s\n", "----", "----", "--------");

        f12_dir_t dir;
        f12_stat_t stat;
        f12_opendir(&fs, "/", &dir);
        int file_count = 0;
        uint32_t total_bytes = 0;

        static uint8_t rbuf[65536];
        while (f12_readdir(&dir, &stat) == F12_OK) {
            uint32_t cksum = 0;
            if (!stat.is_dir && stat.size > 0) {
                f12_file_t *f = f12_open(&fs, stat.name, "r");
                if (f) {
                    uint32_t total = 0;
                    int n;
                    while ((n = f12_read(f, rbuf + total, 512)) > 0) total += n;
                    f12_close(f);
                    cksum = checksum_buf(rbuf, total);
                }
            }
            printf("  %-12s %10lu 0x%08lX%s\n",
                   stat.name, stat.size, cksum, stat.is_dir ? " <DIR>" : "");
            file_count++;
            total_bytes += stat.size;
        }
        f12_closedir(&dir);

        uint16_t free_clusters = 0;
        for (uint16_t c = 2; c < fs.fat.total_clusters + 2; c++) {
            uint16_t next;
            if (fat12_get_entry(&fs.fat, c, &next) == FAT12_OK && next == 0) {
                free_clusters++;
            }
        }

        printf("\n  Files:      %d\n", file_count);
        printf("  Used:       %lu bytes\n", total_bytes);
        printf("  Free:       %lu bytes (%d clusters)\n",
               (uint32_t)free_clusters * bpb->sectors_per_cluster * bpb->bytes_per_sector,
               free_clusters);

        f12_unmount(&fs);
    } else {
        printf("  Not a FAT12 disk (%s)\n", f12_strerror(err));
    }

    printf("\n========================================\n");
    printf("  DONE\n");
    printf("========================================\n");

    for (;;) {
        sleep_ms(10000);
        printf(".\n");
    }

    return 0;
}
