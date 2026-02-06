#include "test.h"
#include "scp_disk.h"
#include "../src/fat12.h"
#include "../src/f12.h"
#include "../src/crc.h"

#define SCP_PATH "../../system-shock-multilingual-floppy-ibm-pc/disk1.scp"

static uint8_t *load_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*size);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, *size, f);
    fclose(f);
    return buf;
}

static uint8_t *scp_file = NULL;
static size_t scp_size = 0;
static scp_disk_t scp_disk;

static uint8_t disk_image[2880][512];
static bool sector_decoded[2880];

static void decode_full_disk(void) {
    flux_sim_t sim;
    flux_sim_open_scp(&sim, scp_file, scp_size);

    memset(sector_decoded, 0, sizeof(sector_decoded));

    for (int track = 0; track < 80; track++) {
        for (int side = 0; side < 2; side++) {
            for (int rev = 0; rev < scp_disk.num_revolutions; rev++) {
                if (!flux_sim_seek(&sim, track, side, rev)) continue;

                mfm_t mfm;
                mfm_init(&mfm);
                sector_t out;
                uint16_t delta;

                while (flux_sim_next(&sim, &delta)) {
                    if (mfm_feed(&mfm, delta, &out)) {
                        if (out.valid && out.track == track && out.side == side &&
                            out.sector_n >= 1 && out.sector_n <= SECTORS_PER_TRACK) {
                            int lba = (track * 2 + side) * SECTORS_PER_TRACK + (out.sector_n - 1);
                            if (!sector_decoded[lba]) {
                                memcpy(disk_image[lba], out.data, 512);
                                sector_decoded[lba] = true;
                            }
                        }
                    }
                }
            }
        }
    }

    flux_sim_close(&sim);
}

static uint32_t checksum_buf(const uint8_t *buf, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (sum << 5) + sum + buf[i];
    }
    return sum;
}

TEST(test_decode_all_sectors) {
    decode_full_disk();

    int decoded = 0;
    for (int i = 0; i < 2880; i++) {
        if (sector_decoded[i]) decoded++;
    }

    printf("\n  Decoded %d/2880 sectors from flux\n  ", decoded);
    ASSERT_EQ(decoded, 2880);
}

TEST(test_boot_sector_validity) {
    uint8_t *boot = disk_image[0];

    ASSERT_EQ(boot[0], 0xEB);
    ASSERT_EQ(boot[510], 0x55);
    ASSERT_EQ(boot[511], 0xAA);

    uint16_t bps = boot[11] | (boot[12] << 8);
    uint8_t spc = boot[13];
    uint16_t reserved = boot[14] | (boot[15] << 8);
    uint8_t fats = boot[16];
    uint16_t root_entries = boot[17] | (boot[18] << 8);
    uint16_t total = boot[19] | (boot[20] << 8);
    uint8_t media = boot[21];
    uint16_t spf = boot[22] | (boot[23] << 8);
    uint16_t spt = boot[24] | (boot[25] << 8);
    uint16_t heads = boot[26] | (boot[27] << 8);

    printf("\n  Boot sector:\n");
    printf("    Bytes/sector:     %d\n", bps);
    printf("    Sectors/cluster:  %d\n", spc);
    printf("    Reserved sectors: %d\n", reserved);
    printf("    FATs:             %d\n", fats);
    printf("    Root entries:     %d\n", root_entries);
    printf("    Total sectors:    %d\n", total);
    printf("    Media descriptor: 0x%02X\n", media);
    printf("    Sectors/FAT:      %d\n", spf);
    printf("    Sectors/track:    %d\n", spt);
    printf("    Heads:            %d\n", heads);
    printf("  ");

    ASSERT_EQ(bps, 512);
    ASSERT_EQ(total, 2880);
    ASSERT_EQ(media, 0xF0);
    ASSERT_EQ(spt, 18);
    ASSERT_EQ(heads, 2);
}

TEST(test_fat_tables_match) {
    uint16_t spf = disk_image[0][22] | (disk_image[0][23] << 8);
    uint16_t fat1_start = 1;
    uint16_t fat2_start = 1 + spf;

    int mismatches = 0;
    for (int i = 0; i < spf; i++) {
        if (memcmp(disk_image[fat1_start + i], disk_image[fat2_start + i], 512) != 0) {
            mismatches++;
        }
    }

    printf("\n  FAT1 (sectors %d-%d) vs FAT2 (sectors %d-%d): %d mismatches\n  ",
           fat1_start, fat1_start + spf - 1,
           fat2_start, fat2_start + spf - 1,
           mismatches);
    ASSERT_EQ(mismatches, 0);
}

TEST(test_fat12_reads_match_raw) {
    fat12_io_t io = {
        .read = scp_disk_read,
        .write = scp_disk_write,
        .ctx = &scp_disk,
    };

    fat12_t fat;
    fat12_init(&fat, io);

    int checked = 0;
    int match = 0;

    for (uint16_t i = 0; i < fat.bpb.root_entries; i++) {
        fat12_dirent_t entry;
        if (fat12_read_root_entry(&fat, i, &entry) != FAT12_OK) break;
        if (fat12_entry_is_end(&entry)) break;
        if (!fat12_entry_valid(&entry)) continue;
        if (entry.attr & 0x18) continue;
        if (entry.size == 0) continue;

        fat12_file_t file;
        if (fat12_open(&fat, &entry, &file) != FAT12_OK) continue;

        uint8_t *buf = malloc(entry.size);
        uint32_t total = 0;
        int n;
        while ((n = fat12_read(&file, buf + total, 512)) > 0) {
            total += n;
        }

        uint16_t cluster = entry.start_cluster;
        uint32_t offset = 0;
        bool data_match = true;

        while (cluster >= 2 && !fat12_is_eof(cluster) && offset < total) {
            uint16_t lba = fat.data_start_sector + (cluster - 2);
            uint16_t chunk = total - offset;
            if (chunk > 512) chunk = 512;

            if (memcmp(buf + offset, disk_image[lba], chunk) != 0) {
                data_match = false;
            }

            offset += 512;
            uint16_t next;
            fat12_get_entry(&fat, cluster, &next);
            cluster = next;
        }

        checked++;
        if (data_match && total == entry.size) match++;

        free(buf);
    }

    printf("\n  FAT12 file reads vs raw sectors: %d/%d match\n  ", match, checked);
    ASSERT_EQ(match, checked);
}

typedef struct {
    char name[13];
    uint32_t size;
    uint32_t checksum;
} file_record_t;

TEST(test_f12_list_all_files) {
    f12_io_t io = {
        .read = scp_disk_read,
        .write = scp_disk_write,
        .disk_changed = NULL,
        .write_protected = scp_disk_write_protected,
        .ctx = &scp_disk,
    };

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    f12_err_t err = f12_mount(&fs, io);
    ASSERT_EQ(err, F12_OK);

    printf("\n  Files on System Shock Disk 1:\n");
    printf("  %-12s %10s %10s\n", "NAME", "SIZE", "CHECKSUM");
    printf("  %-12s %10s %10s\n", "----", "----", "--------");

    f12_dir_t dir;
    f12_opendir(&fs, "/", &dir);

    int file_count = 0;
    uint32_t total_bytes = 0;
    f12_stat_t stat;

    while (f12_readdir(&dir, &stat) == F12_OK) {
        if (stat.is_dir) continue;

        f12_file_t *f = f12_open(&fs, stat.name, "r");
        ASSERT(f != NULL);

        uint8_t *buf = malloc(stat.size + 1);
        uint32_t total = 0;
        int n;
        while ((n = f12_read(f, buf + total, 512)) > 0) {
            total += n;
        }
        f12_close(f);

        ASSERT_EQ(total, stat.size);

        uint32_t cksum = checksum_buf(buf, total);
        printf("  %-12s %10u 0x%08X\n", stat.name, total, cksum);

        free(buf);
        file_count++;
        total_bytes += total;
    }
    f12_closedir(&dir);

    printf("  ---\n");
    printf("  %d files, %u bytes total\n  ", file_count, total_bytes);
    ASSERT(file_count > 0);

    f12_unmount(&fs);
}

TEST(test_f12_read_consistency) {
    f12_io_t io = {
        .read = scp_disk_read,
        .write = scp_disk_write,
        .disk_changed = NULL,
        .write_protected = scp_disk_write_protected,
        .ctx = &scp_disk,
    };

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    f12_mount(&fs, io);

    f12_dir_t dir;
    f12_opendir(&fs, "/", &dir);

    f12_stat_t stat;
    int verified = 0;

    while (f12_readdir(&dir, &stat) == F12_OK) {
        if (stat.is_dir || stat.size == 0) continue;

        uint8_t *buf1 = malloc(stat.size);
        uint8_t *buf2 = malloc(stat.size);

        f12_file_t *f1 = f12_open(&fs, stat.name, "r");
        uint32_t t1 = 0;
        int n;
        while ((n = f12_read(f1, buf1 + t1, 512)) > 0) t1 += n;
        f12_close(f1);

        f12_file_t *f2 = f12_open(&fs, stat.name, "r");
        uint32_t t2 = 0;
        while ((n = f12_read(f2, buf2 + t2, 512)) > 0) t2 += n;
        f12_close(f2);

        ASSERT_EQ(t1, stat.size);
        ASSERT_EQ(t2, stat.size);
        ASSERT_MEM_EQ(buf1, buf2, stat.size);

        free(buf1);
        free(buf2);
        verified++;
    }
    f12_closedir(&dir);

    printf("\n  %d files read twice and verified identical\n  ", verified);
    ASSERT(verified > 0);

    f12_unmount(&fs);
}

TEST(test_disk_image_checksum) {
    uint32_t cksum = 0;
    for (int i = 0; i < 2880; i++) {
        cksum ^= checksum_buf(disk_image[i], 512);
    }

    printf("\n  Full disk image checksum: 0x%08X\n  ", cksum);
    ASSERT(cksum != 0);
}

int main(int argc, char *argv[]) {
    const char *scp_path = SCP_PATH;
    if (argc > 1) scp_path = argv[1];

    scp_file = load_file(scp_path, &scp_size);
    if (!scp_file) {
        printf("=== SCP+FAT12 Tests: SKIPPED (no SCP file at %s) ===\n", scp_path);
        return 0;
    }

    scp_disk_init(&scp_disk, scp_file, scp_size);

    printf("=== SCP+FAT12 End-to-End Tests ===\n");
    printf("File: %s (%.1f MB)\n\n", scp_path, scp_size / 1048576.0);

    printf("--- Raw Decode ---\n");
    RUN_TEST(test_decode_all_sectors);
    RUN_TEST(test_boot_sector_validity);
    RUN_TEST(test_fat_tables_match);
    RUN_TEST(test_disk_image_checksum);

    printf("\n--- FAT12 Layer ---\n");
    RUN_TEST(test_fat12_reads_match_raw);

    printf("\n--- F12 High-Level API ---\n");
    RUN_TEST(test_f12_list_all_files);
    RUN_TEST(test_f12_read_consistency);

    free(scp_file);

    TEST_RESULTS();
}
