#include <time.h>
#include "test.h"
#include "scp_disk.h"
#include "vdisk.h"
#include "../src/fat12.h"
#include "../src/f12.h"

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

static uint8_t disk_original[2880][512];
static uint8_t disk_modified[2880][512];
static uint8_t disk_roundtrip[2880][512];
static vdisk_t shared_vdisk;
static vdisk_t shared_vdisk2;
static char renamed_src[13];
static uint32_t renamed_size;
static uint32_t renamed_checksum;

static void decode_scp_to_image(uint8_t *scp_data, size_t scp_size,
                                 uint8_t image[2880][512]) {
    flux_sim_t sim;
    flux_sim_open_scp(&sim, scp_data, scp_size);

    for (int track = 0; track < 80; track++) {
        for (int side = 0; side < 2; side++) {
            for (int rev = 0; rev < sim.num_revolutions; rev++) {
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
                            memcpy(image[lba], out.data, 512);
                        }
                    }
                }
            }
        }
    }

    flux_sim_close(&sim);
}

static void image_to_vdisk(const uint8_t image[2880][512], vdisk_t *disk) {
    memset(disk, 0, sizeof(*disk));
    memcpy(disk->data, image, 2880 * 512);
}

static void vdisk_to_image(const vdisk_t *disk, uint8_t image[2880][512]) {
    memcpy(image, disk->data, sizeof(disk->data));
}

static uint32_t checksum_buf(const uint8_t *buf, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (sum << 5) + sum + buf[i];
    }
    return sum;
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
    ASSERT_EQ(written, len);
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

static void f12_delete_if_present(f12_t *fs, const char *name) {
    f12_stat_t stat;
    if (f12_stat(fs, name, &stat) == F12_OK) {
        ASSERT_EQ(f12_delete(fs, name), F12_OK);
    }
}

TEST(test_decode_original) {
    size_t scp_size;
    uint8_t *scp_data = load_file(SCP_PATH, &scp_size);
    ASSERT(scp_data != NULL);

    decode_scp_to_image(scp_data, scp_size, disk_original);
    free(scp_data);

    ASSERT_EQ(disk_original[0][510], 0x55);
    ASSERT_EQ(disk_original[0][511], 0xAA);
    printf("\n  Decoded 2880 sectors, checksum: 0x%08X\n  ",
           checksum_buf((uint8_t *)disk_original, sizeof(disk_original)));
}

TEST(test_f12_heavy_modifications) {
    memcpy(disk_modified, disk_original, sizeof(disk_original));
    image_to_vdisk(disk_modified, &shared_vdisk);

    f12_io_t io = {
        .read = vdisk_read, .write = vdisk_write,
        .disk_changed = vdisk_disk_changed,
        .write_protected = vdisk_write_protected,
        .ctx = &shared_vdisk,
    };

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    ASSERT_EQ(f12_mount(&fs, io), F12_OK);

    printf("\n");

    f12_stat_t stat;
    f12_err_t err;
    f12_file_t *f;
    int n;

    f = f12_open(&fs, "README.SS", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "README.SS", &stat);
    uint8_t *readme_content = malloc(stat.size + 1);
    uint32_t readme_size = f12_read_full(f, readme_content, stat.size);
    ASSERT_EQ(readme_size, stat.size);
    readme_content[stat.size] = '\0';
    ASSERT_EQ(f12_close(f), F12_OK);

    char *target = NULL;
    for (uint32_t i = 0; i + 6 <= readme_size; i++) {
        if (memcmp(readme_content + i, "System", 6) == 0) {
            target = (char *)readme_content + i;
            break;
        }
    }
    ASSERT(target != NULL);
    memcpy(target, "Floppy", 6);

    f = f12_open(&fs, "README.SS", "w");
    ASSERT(f != NULL);
    f12_write_full(f, readme_content, readme_size);
    ASSERT_EQ(f12_close(f), F12_OK);
    printf("  [MODIFY] README.SS: 'System' -> 'Floppy'\n");

    f12_stat(&fs, "README.SS", &stat);
    ASSERT_EQ(stat.size, readme_size);

    err = f12_delete(&fs, "LHA.DOC");
    ASSERT_EQ(err, F12_OK);
    printf("  [DELETE] LHA.DOC (31840 bytes)\n");

    err = f12_stat(&fs, "LHA.DOC", &stat);
    ASSERT_EQ(err, F12_ERR_NOT_FOUND);

    err = f12_delete(&fs, "SHOCKGUS.BAT");
    ASSERT_EQ(err, F12_OK);
    printf("  [DELETE] SHOCKGUS.BAT (123 bytes)\n");

    f = f12_open(&fs, "HELLO.TXT", "w");
    ASSERT(f != NULL);
    const char *hello = "Hello from the floppy controller! This file was created by our FAT12 implementation.";
    f12_write_full(f, hello, strlen(hello));
    ASSERT_EQ(f12_close(f), F12_OK);
    printf("  [CREATE] HELLO.TXT (%zu bytes)\n", strlen(hello));

    f12_stat(&fs, "HELLO.TXT", &stat);
    ASSERT_EQ(stat.size, (uint32_t)strlen(hello));

    f = f12_open(&fs, "BIG.DAT", "w");
    ASSERT(f != NULL);
    uint8_t pattern[10000];
    for (int i = 0; i < 10000; i++) pattern[i] = (i * 7 + 13) & 0xFF;
    f12_write_full(f, pattern, sizeof(pattern));
    ASSERT_EQ(f12_close(f), F12_OK);
    printf("  [CREATE] BIG.DAT (10000 bytes)\n");

    f12_stat(&fs, "BIG.DAT", &stat);
    ASSERT_EQ(stat.size, 10000);

    f = f12_open(&fs, "TINY.BIN", "w");
    ASSERT(f != NULL);
    uint8_t one_byte = 0x42;
    n = f12_write(f, &one_byte, 1);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(f12_close(f), F12_OK);
    printf("  [CREATE] TINY.BIN (1 byte)\n");

    f = f12_open(&fs, "EMPTY.TXT", "w");
    ASSERT(f != NULL);
    ASSERT_EQ(f12_close(f), F12_OK);
    printf("  [CREATE] EMPTY.TXT (0 bytes)\n");

    f = f12_open(&fs, "CYB.CFG", "w");
    ASSERT(f != NULL);
    const char *new_cfg = "overwritten_config_data_here_1234567890";
    f12_write_full(f, new_cfg, strlen(new_cfg));
    ASSERT_EQ(f12_close(f), F12_OK);
    printf("  [OVERWRITE] CYB.CFG: 54 -> %zu bytes\n", strlen(new_cfg));

    f12_stat(&fs, "CYB.CFG", &stat);
    ASSERT_EQ(stat.size, (uint32_t)strlen(new_cfg));

    f12_dir_t scan;
    f12_opendir(&fs, "/", &scan);
    renamed_src[0] = '\0';
    while (f12_readdir(&scan, &stat) == F12_OK) {
        if (strcmp(stat.name, "README.SS") == 0) continue;
        if (strcmp(stat.name, "CYB.CFG") == 0) continue;
        if (strcmp(stat.name, "INSTALL.EXE") == 0) continue;
        if (strcmp(stat.name, "BASE.LZH") == 0) continue;
        if (strcmp(stat.name, "HELLO.TXT") == 0) continue;
        if (strcmp(stat.name, "BIG.DAT") == 0) continue;
        if (strcmp(stat.name, "TINY.BIN") == 0) continue;
        if (strcmp(stat.name, "EMPTY.TXT") == 0) continue;
        if (stat.size == 0 || stat.size > 200000) continue;
        strcpy(renamed_src, stat.name);
        renamed_size = stat.size;
        break;
    }
    f12_closedir(&scan);
    ASSERT(renamed_src[0] != '\0');

    f = f12_open(&fs, renamed_src, "r");
    ASSERT(f != NULL);
    uint8_t *victim_before = malloc(renamed_size);
    uint32_t got = f12_read_full(f, victim_before, renamed_size);
    ASSERT_EQ(f12_close(f), F12_OK);
    ASSERT_EQ(got, renamed_size);
    renamed_checksum = checksum_buf(victim_before, renamed_size);

    ASSERT_EQ(f12_rename(&fs, renamed_src, "RENAMED.OLD"), F12_OK);
    printf("  [RENAME] %s -> RENAMED.OLD (%u bytes, 0x%08X)\n",
           renamed_src, renamed_size, renamed_checksum);

    ASSERT_EQ(f12_stat(&fs, renamed_src, &stat), F12_ERR_NOT_FOUND);
    ASSERT_EQ(f12_stat(&fs, "RENAMED.OLD", &stat), F12_OK);
    ASSERT_EQ(stat.size, renamed_size);

    f = f12_open(&fs, "RENAMED.OLD", "r");
    ASSERT(f != NULL);
    uint8_t *victim_after = malloc(renamed_size);
    got = f12_read_full(f, victim_after, renamed_size);
    ASSERT_EQ(f12_close(f), F12_OK);
    ASSERT_EQ(got, renamed_size);
    ASSERT_MEM_EQ(victim_before, victim_after, renamed_size);
    free(victim_before);
    free(victim_after);

    ASSERT_EQ(f12_rename(&fs, "HELLO.TXT", "GREET.TXT"), F12_OK);
    printf("  [RENAME] HELLO.TXT -> GREET.TXT\n");
    ASSERT_EQ(f12_rename(&fs, "GREET.TXT", "CYB.CFG"), F12_ERR_EXISTS);
    ASSERT_EQ(f12_rename(&fs, "HELLO.TXT", "AGAIN.TXT"), F12_ERR_NOT_FOUND);

    printf("\n  --- Verify all modifications in vdisk ---\n");

    f = f12_open(&fs, "README.SS", "r");
    ASSERT(f != NULL);
    uint8_t *check = malloc(readme_size + 1);
    uint32_t t = f12_read_full(f, check, readme_size);
    ASSERT_EQ(f12_close(f), F12_OK);
    ASSERT_EQ(t, readme_size);
    ASSERT_MEM_EQ(readme_content, check, readme_size);
    printf("  README.SS: content verified\n");
    free(check);

    err = f12_stat(&fs, "LHA.DOC", &stat);
    ASSERT_EQ(err, F12_ERR_NOT_FOUND);
    printf("  LHA.DOC: confirmed deleted\n");

    err = f12_stat(&fs, "SHOCKGUS.BAT", &stat);
    ASSERT_EQ(err, F12_ERR_NOT_FOUND);
    printf("  SHOCKGUS.BAT: confirmed deleted\n");

    f = f12_open(&fs, "GREET.TXT", "r");
    ASSERT(f != NULL);
    char hello_buf[256] = {0};
    t = f12_read_full(f, hello_buf, sizeof(hello_buf));
    ASSERT_EQ(f12_close(f), F12_OK);
    ASSERT_EQ(t, (uint32_t)strlen(hello));
    ASSERT(memcmp(hello_buf, hello, strlen(hello)) == 0);
    printf("  GREET.TXT: content verified (was HELLO.TXT)\n");

    f = f12_open(&fs, "BIG.DAT", "r");
    ASSERT(f != NULL);
    uint8_t *big_buf = malloc(10000);
    t = f12_read_full(f, big_buf, 10000);
    ASSERT_EQ(f12_close(f), F12_OK);
    ASSERT_EQ(t, 10000);
    ASSERT_MEM_EQ(big_buf, pattern, 10000);
    printf("  BIG.DAT: 10000 bytes verified\n");
    free(big_buf);

    f = f12_open(&fs, "TINY.BIN", "r");
    ASSERT(f != NULL);
    uint8_t tiny_buf;
    n = f12_read(f, &tiny_buf, 1);
    ASSERT_EQ(f12_close(f), F12_OK);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(tiny_buf, 0x42);
    printf("  TINY.BIN: 1 byte verified\n");

    f12_stat(&fs, "EMPTY.TXT", &stat);
    ASSERT_EQ(stat.size, 0);
    printf("  EMPTY.TXT: 0 bytes confirmed\n");

    f = f12_open(&fs, "CYB.CFG", "r");
    ASSERT(f != NULL);
    char cfg_buf[256] = {0};
    t = f12_read_full(f, cfg_buf, sizeof(cfg_buf));
    ASSERT_EQ(f12_close(f), F12_OK);
    ASSERT_EQ(t, (uint32_t)strlen(new_cfg));
    ASSERT(memcmp(cfg_buf, new_cfg, strlen(new_cfg)) == 0);
    printf("  CYB.CFG: overwritten content verified\n");

    f = f12_open(&fs, "INSTALL.EXE", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "INSTALL.EXE", &stat);
    uint8_t *install_buf = malloc(stat.size);
    t = f12_read_full(f, install_buf, stat.size);
    ASSERT_EQ(f12_close(f), F12_OK);
    ASSERT_EQ(t, stat.size);
    printf("  INSTALL.EXE: %u bytes readable (untouched)\n", t);
    ASSERT_EQ(checksum_buf(install_buf, t), 0xD3FA0E4E);
    printf("  INSTALL.EXE: checksum 0xD3FA0E4E verified\n");
    free(install_buf);

    printf("\n  --- Directory listing after modifications ---\n");
    f12_dir_t dir;
    f12_opendir(&fs, "/", &dir);
    int count = 0;
    while (f12_readdir(&dir, &stat) == F12_OK) {
        printf("  %-12s %8u bytes\n", stat.name, stat.size);
        count++;
    }
    f12_closedir(&dir);
    printf("  Total: %d files\n  ", count);

    ASSERT(count >= 12);

    free(readme_content);
    vdisk_to_image(&shared_vdisk, disk_modified);
    f12_unmount(&fs);
}

TEST(test_encode_to_scp) {
    size_t scp_size;
    uint8_t *scp_data = scp_encode_disk(disk_modified, &scp_size);
    ASSERT(scp_data != NULL);
    printf("\n  Encoded SCP: %.1f MB\n", scp_size / 1048576.0);

    FILE *f = fopen("disk1-ours.scp", "wb");
    ASSERT(f != NULL);
    fwrite(scp_data, 1, scp_size, f);
    fclose(f);
    printf("  Written to disk1-ours.scp\n  ");
    free(scp_data);
}

TEST(test_decode_roundtrip) {
    size_t scp_size;
    uint8_t *scp_data = load_file("disk1-ours.scp", &scp_size);
    ASSERT(scp_data != NULL);

    decode_scp_to_image(scp_data, scp_size, disk_roundtrip);
    free(scp_data);

    int match = 0;
    for (int i = 0; i < 2880; i++) {
        if (memcmp(disk_modified[i], disk_roundtrip[i], 512) == 0) match++;
    }
    printf("\n  Sectors: %d/2880 match\n  ", match);
    ASSERT_EQ(match, 2880);
}

TEST(test_verify_all_after_roundtrip) {
    image_to_vdisk(disk_roundtrip, &shared_vdisk);

    f12_io_t io = {
        .read = vdisk_read, .write = vdisk_write,
        .disk_changed = vdisk_disk_changed,
        .write_protected = vdisk_write_protected,
        .ctx = &shared_vdisk,
    };

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    ASSERT_EQ(f12_mount(&fs, io), F12_OK);

    f12_stat_t stat;
    f12_file_t *f;
    printf("\n");

    f = f12_open(&fs, "README.SS", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "README.SS", &stat);
    char *readme = malloc(stat.size + 1);
    uint32_t t = f12_read_full(f, readme, stat.size);
    ASSERT_EQ(f12_close(f), F12_OK);
    readme[t] = '\0';
    char *found = strstr(readme, "Floppy");
    ASSERT(found != NULL);
    printf("  README.SS: 'Floppy' found at offset %ld\n", found - readme);
    free(readme);

    ASSERT_EQ(f12_stat(&fs, "LHA.DOC", &stat), F12_ERR_NOT_FOUND);
    printf("  LHA.DOC: confirmed deleted\n");

    ASSERT_EQ(f12_stat(&fs, "SHOCKGUS.BAT", &stat), F12_ERR_NOT_FOUND);
    printf("  SHOCKGUS.BAT: confirmed deleted\n");

    ASSERT_EQ(f12_stat(&fs, "HELLO.TXT", &stat), F12_ERR_NOT_FOUND);
    f = f12_open(&fs, "GREET.TXT", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "GREET.TXT", &stat);
    char *hello = malloc(stat.size + 1);
    t = f12_read_full(f, hello, stat.size);
    ASSERT_EQ(f12_close(f), F12_OK);
    hello[t] = '\0';
    ASSERT(strstr(hello, "floppy controller") != NULL);
    printf("  GREET.TXT: content survived rename (%u bytes)\n", t);
    free(hello);

    ASSERT_EQ(f12_stat(&fs, renamed_src, &stat), F12_ERR_NOT_FOUND);
    f = f12_open(&fs, "RENAMED.OLD", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "RENAMED.OLD", &stat);
    ASSERT_EQ(stat.size, renamed_size);
    uint8_t *renamed = malloc(renamed_size);
    t = f12_read_full(f, renamed, renamed_size);
    ASSERT_EQ(f12_close(f), F12_OK);
    ASSERT_EQ(t, renamed_size);
    ASSERT_EQ(checksum_buf(renamed, t), renamed_checksum);
    printf("  RENAMED.OLD: %u bytes, checksum 0x%08X verified (was %s)\n",
           t, renamed_checksum, renamed_src);
    free(renamed);

    f = f12_open(&fs, "BIG.DAT", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "BIG.DAT", &stat);
    ASSERT_EQ(stat.size, 10000);
    uint8_t *big = malloc(10000);
    t = f12_read_full(f, big, 10000);
    ASSERT_EQ(f12_close(f), F12_OK);
    ASSERT_EQ(t, 10000);
    uint8_t expected[10000];
    for (int i = 0; i < 10000; i++) expected[i] = (i * 7 + 13) & 0xFF;
    ASSERT_MEM_EQ(big, expected, 10000);
    printf("  BIG.DAT: 10000 bytes verified\n");
    free(big);

    f = f12_open(&fs, "TINY.BIN", "r");
    ASSERT(f != NULL);
    uint8_t byte;
    ASSERT_EQ(f12_read(f, &byte, 1), 1);
    ASSERT_EQ(byte, 0x42);
    ASSERT_EQ(f12_close(f), F12_OK);
    printf("  TINY.BIN: 0x42 verified\n");

    f12_stat(&fs, "EMPTY.TXT", &stat);
    ASSERT_EQ(stat.size, 0);
    printf("  EMPTY.TXT: 0 bytes confirmed\n");

    f = f12_open(&fs, "CYB.CFG", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "CYB.CFG", &stat);
    char *cfg = malloc(stat.size + 1);
    t = f12_read_full(f, cfg, stat.size);
    ASSERT_EQ(f12_close(f), F12_OK);
    cfg[t] = '\0';
    ASSERT(strstr(cfg, "overwritten_config") != NULL);
    printf("  CYB.CFG: overwritten content survived (%u bytes)\n", t);
    free(cfg);

    f = f12_open(&fs, "INSTALL.EXE", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "INSTALL.EXE", &stat);
    uint8_t *exe = malloc(stat.size);
    t = f12_read_full(f, exe, stat.size);
    ASSERT_EQ(f12_close(f), F12_OK);
    ASSERT_EQ(checksum_buf(exe, t), 0xD3FA0E4E);
    printf("  INSTALL.EXE: checksum 0xD3FA0E4E (untouched)\n");
    free(exe);

    f = f12_open(&fs, "BASE.LZH", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "BASE.LZH", &stat);
    uint8_t *lzh = malloc(stat.size);
    t = f12_read_full(f, lzh, stat.size);
    ASSERT_EQ(f12_close(f), F12_OK);
    ASSERT_EQ(checksum_buf(lzh, t), 0x49532436);
    printf("  BASE.LZH: checksum 0x49532436 (untouched)\n");
    free(lzh);

    printf("\n  --- Final directory ---\n");
    f12_dir_t dir;
    f12_opendir(&fs, "/", &dir);
    int count = 0;
    while (f12_readdir(&dir, &stat) == F12_OK) {
        printf("  %-12s %8u  0x%08X\n", stat.name, stat.size,
               stat.size > 0 ? (unsigned)stat.size : 0);
        count++;
    }
    f12_closedir(&dir);
    printf("  Total: %d files\n  ", count);
    ASSERT(count >= 12);

    f12_unmount(&fs);
}

typedef struct {
    char name[13];
    uint32_t size;
    uint32_t checksum;
} file_manifest_t;

#define MAX_MANIFEST 224

static uint32_t gen_pattern_byte(int file_id, uint32_t offset) {
    uint32_t v = file_id * 2654435761u + offset * 40503u;
    return (v >> 16) & 0xFF;
}

static f12_io_t make_vdisk_io(vdisk_t *v) {
    return (f12_io_t){
        .read = vdisk_read, .write = vdisk_write,
        .disk_changed = vdisk_disk_changed,
        .write_protected = vdisk_write_protected,
        .ctx = v,
    };
}

TEST(test_format_fill_max_roundtrip) {
    vdisk_init(&shared_vdisk);

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    fs.io = make_vdisk_io(&shared_vdisk);

    f12_err_t err = f12_format(&fs, "FULLTEST", true);
    ASSERT_EQ(err, F12_OK);

    err = f12_mount(&fs, make_vdisk_io(&shared_vdisk));
    ASSERT_EQ(err, F12_OK);

    printf("\n  Formatted fresh 1.44MB floppy\n");

    file_manifest_t manifest[MAX_MANIFEST];
    int manifest_count = 0;
    uint32_t total_written = 0;
    int files_created = 0;

    uint8_t *wbuf = malloc(120000);

    for (int file_id = 0; file_id < 100; file_id++) {
        char name[13];
        uint32_t target_size;

        if (file_id < 10) {
            snprintf(name, sizeof(name), "HUGE%d.DAT", file_id);
            target_size = 65000 + file_id * 5000;
        } else if (file_id < 30) {
            snprintf(name, sizeof(name), "MED%02d.DAT", file_id);
            target_size = 15000 + file_id * 500;
        } else if (file_id < 60) {
            snprintf(name, sizeof(name), "SM%02d.DAT", file_id);
            target_size = 2000 + file_id * 100;
        } else {
            snprintf(name, sizeof(name), "T%02d.BIN", file_id);
            target_size = 100 + file_id * 5;
        }

        f12_file_t *f = f12_open(&fs, name, "w");
        if (!f) break;

        uint32_t written = 0;
        bool write_ok = true;
        while (written < target_size) {
            uint32_t chunk = target_size - written;
            if (chunk > 512) chunk = 512;
            for (uint32_t i = 0; i < chunk; i++) {
                wbuf[i] = gen_pattern_byte(file_id, written + i);
            }
            int n = f12_write(f, wbuf, chunk);
            if (n <= 0) { write_ok = false; break; }
            written += n;
        }
        f12_err_t close_err = f12_close(f);
        if (write_ok) {
            ASSERT_EQ(close_err, F12_OK);
        } else {
            ASSERT(close_err == F12_OK || close_err == F12_ERR_FULL);
        }

        if (!write_ok || close_err != F12_OK || written == 0) {
            f12_delete_if_present(&fs, name);
            break;
        }

        uint32_t cksum = 0;
        for (uint32_t i = 0; i < written; i++) {
            uint8_t b = gen_pattern_byte(file_id, i);
            cksum = (cksum << 5) + cksum + b;
        }

        memcpy(manifest[manifest_count].name, name, 13);
        manifest[manifest_count].size = written;
        manifest[manifest_count].checksum = cksum;
        manifest_count++;

        total_written += written;
        files_created++;
    }

    printf("  Created %d files, %u bytes total (%.1f%% of 1.44MB)\n",
           files_created, total_written, total_written * 100.0 / 1457664.0);
    ASSERT(files_created >= 20);
    ASSERT(total_written > 1000000);

    printf("  Verifying in vdisk before encode...\n");
    int verified = 0;
    for (int i = 0; i < manifest_count; i++) {
        f12_file_t *f = f12_open(&fs, manifest[i].name, "r");
        ASSERT(f != NULL);
        uint32_t t = f12_read_full(f, wbuf, manifest[i].size);
        ASSERT_EQ(f12_close(f), F12_OK);
        ASSERT_EQ(t, manifest[i].size);
        ASSERT_EQ(checksum_buf(wbuf, t), manifest[i].checksum);
        verified++;
    }
    printf("  %d/%d files verified\n", verified, manifest_count);

    f12_unmount(&fs);

    vdisk_to_image(&shared_vdisk, disk_modified);

    printf("  Encoding to SCP...\n");
    size_t scp_size;
    uint8_t *scp_data = scp_encode_disk(disk_modified, &scp_size);
    ASSERT(scp_data != NULL);
    printf("  SCP: %.1f MB\n", scp_size / 1048576.0);

    printf("  Decoding from SCP...\n");
    flux_sim_t sim;
    flux_sim_open_scp(&sim, scp_data, scp_size);

    memset(disk_roundtrip, 0, sizeof(disk_roundtrip));
    for (int track = 0; track < 80; track++) {
        for (int side = 0; side < 2; side++) {
            if (!flux_sim_seek(&sim, track, side, 0)) continue;
            mfm_t mfm;
            mfm_init(&mfm);
            sector_t out;
            uint16_t delta;
            while (flux_sim_next(&sim, &delta)) {
                if (mfm_feed(&mfm, delta, &out)) {
                    if (out.valid && out.track == track && out.side == side &&
                        out.sector_n >= 1 && out.sector_n <= SECTORS_PER_TRACK) {
                        int lba = (track * 2 + side) * SECTORS_PER_TRACK + (out.sector_n - 1);
                        memcpy(disk_roundtrip[lba], out.data, 512);
                    }
                }
            }
        }
    }
    flux_sim_close(&sim);
    free(scp_data);

    int sector_match = 0;
    for (int i = 0; i < 2880; i++) {
        if (memcmp(disk_modified[i], disk_roundtrip[i], 512) == 0) sector_match++;
    }
    printf("  Sectors: %d/2880 match\n", sector_match);
    ASSERT_EQ(sector_match, 2880);

    image_to_vdisk(disk_roundtrip, &shared_vdisk);
    err = f12_mount(&fs, make_vdisk_io(&shared_vdisk));
    ASSERT_EQ(err, F12_OK);

    printf("  Verifying %d files after roundtrip...\n", manifest_count);
    int pass = 0;
    int fail = 0;
    for (int i = 0; i < manifest_count; i++) {
        f12_file_t *f = f12_open(&fs, manifest[i].name, "r");
        if (!f) { fail++; continue; }
        uint32_t t = f12_read_full(f, wbuf, manifest[i].size);
        ASSERT_EQ(f12_close(f), F12_OK);
        if (t != manifest[i].size || checksum_buf(wbuf, t) != manifest[i].checksum) {
            printf("  FAIL: %s (size %u/%u, cksum 0x%08X/0x%08X)\n",
                   manifest[i].name, t, manifest[i].size,
                   checksum_buf(wbuf, t), manifest[i].checksum);
            fail++;
        } else {
            pass++;
        }
    }
    printf("  Result: %d/%d files verified, %d failed\n", pass, manifest_count, fail);
    ASSERT_EQ(fail, 0);

    int tracks_with_data[160] = {0};
    for (int i = 0; i < 2880; i++) {
        bool empty = true;
        for (int j = 0; j < 512; j++) {
            if (disk_roundtrip[i][j] != 0) { empty = false; break; }
        }
        if (!empty) tracks_with_data[i / 18]++;
    }
    int used_tracks = 0;
    for (int i = 0; i < 160; i++) {
        if (tracks_with_data[i] > 0) used_tracks++;
    }
    printf("  Tracks with data: %d/160 (both sides)\n  ", used_tracks);
    ASSERT(used_tracks >= 100);

    free(wbuf);
    f12_unmount(&fs);
}

TEST(test_format_fill_delete_refill_roundtrip) {
    vdisk_init(&shared_vdisk);

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    fs.io = make_vdisk_io(&shared_vdisk);

    ASSERT_EQ(f12_format(&fs, "CHURN", true), F12_OK);
    ASSERT_EQ(f12_mount(&fs, make_vdisk_io(&shared_vdisk)), F12_OK);

    printf("\n");
    uint8_t *wbuf = malloc(65536);

    printf("  Phase A: Fill with 50 files...\n");
    for (int i = 0; i < 50; i++) {
        char name[13];
        snprintf(name, sizeof(name), "FILL%02d.DAT", i);
        f12_file_t *f = f12_open(&fs, name, "w");
        if (!f) break;
        uint32_t sz = 5000 + i * 200;
        for (uint32_t j = 0; j < sz; j++) wbuf[j] = gen_pattern_byte(i + 1000, j);
        f12_write_full(f, wbuf, sz);
        ASSERT_EQ(f12_close(f), F12_OK);
    }

    f12_dir_t dir;
    f12_stat_t stat;
    f12_opendir(&fs, "/", &dir);
    int count_a = 0;
    while (f12_readdir(&dir, &stat) == F12_OK) count_a++;
    f12_closedir(&dir);
    printf("  Files after fill: %d\n", count_a);

    printf("  Phase B: Delete every other file...\n");
    int deleted = 0;
    for (int i = 0; i < 50; i += 2) {
        char name[13];
        snprintf(name, sizeof(name), "FILL%02d.DAT", i);
        if (f12_delete(&fs, name) == F12_OK) deleted++;
    }
    printf("  Deleted: %d files\n", deleted);

    printf("  Phase C: Refill with new files in freed space...\n");
    file_manifest_t manifest[100];
    int mc = 0;
    for (int i = 0; i < 80; i++) {
        char name[13];
        snprintf(name, sizeof(name), "NEW%03d.BIN", i);
        uint32_t sz = 3000 + i * 100;
        f12_file_t *f = f12_open(&fs, name, "w");
        if (!f) break;
        for (uint32_t j = 0; j < sz; j++) wbuf[j] = gen_pattern_byte(i + 2000, j);
        uint32_t written = 0;
        bool write_ok = true;
        while (written < sz) {
            uint32_t chunk = sz - written;
            if (chunk > 512) chunk = 512;
            int n = f12_write(f, wbuf + written, chunk);
            if (n <= 0) { write_ok = false; break; }
            written += n;
        }
        f12_err_t close_err = f12_close(f);
        if (write_ok) {
            ASSERT_EQ(close_err, F12_OK);
        } else {
            ASSERT(close_err == F12_OK || close_err == F12_ERR_FULL);
        }
        if (!write_ok || close_err != F12_OK || written == 0) {
            f12_delete_if_present(&fs, name);
            break;
        }

        memcpy(manifest[mc].name, name, 13);
        manifest[mc].size = written;
        uint32_t ck = 0;
        for (uint32_t j = 0; j < written; j++) {
            ck = (ck << 5) + ck + gen_pattern_byte(i + 2000, j);
        }
        manifest[mc].checksum = ck;
        mc++;
    }
    printf("  Created %d new files\n", mc);

    for (int i = 1; i < 50; i += 2) {
        char name[13];
        snprintf(name, sizeof(name), "FILL%02d.DAT", i);
        f12_stat_t st;
        ASSERT_EQ(f12_stat(&fs, name, &st), F12_OK);
    }
    printf("  Surviving FILL files verified present\n");

    f12_unmount(&fs);
    vdisk_to_image(&shared_vdisk, disk_modified);

    size_t scp_size;
    uint8_t *scp_data = scp_encode_disk(disk_modified, &scp_size);
    ASSERT(scp_data != NULL);
    printf("  Encoded SCP: %.1f MB\n", scp_size / 1048576.0);

    flux_sim_t sim;
    flux_sim_open_scp(&sim, scp_data, scp_size);
    memset(disk_roundtrip, 0, sizeof(disk_roundtrip));
    for (int track = 0; track < 80; track++) {
        for (int side = 0; side < 2; side++) {
            if (!flux_sim_seek(&sim, track, side, 0)) continue;
            mfm_t mfm;
            mfm_init(&mfm);
            sector_t out;
            uint16_t delta;
            while (flux_sim_next(&sim, &delta)) {
                if (mfm_feed(&mfm, delta, &out)) {
                    if (out.valid && out.track == track && out.side == side &&
                        out.sector_n >= 1 && out.sector_n <= SECTORS_PER_TRACK) {
                        int lba = (track * 2 + side) * SECTORS_PER_TRACK + (out.sector_n - 1);
                        memcpy(disk_roundtrip[lba], out.data, 512);
                    }
                }
            }
        }
    }
    flux_sim_close(&sim);
    free(scp_data);

    int smatch = 0;
    for (int i = 0; i < 2880; i++) {
        if (memcmp(disk_modified[i], disk_roundtrip[i], 512) == 0) smatch++;
    }
    printf("  Sectors: %d/2880 match\n", smatch);
    ASSERT_EQ(smatch, 2880);

    image_to_vdisk(disk_roundtrip, &shared_vdisk);
    f12_err_t err = f12_mount(&fs, make_vdisk_io(&shared_vdisk));
    ASSERT_EQ(err, F12_OK);

    int pass = 0;
    for (int i = 0; i < mc; i++) {
        f12_file_t *f = f12_open(&fs, manifest[i].name, "r");
        ASSERT(f != NULL);
        uint32_t t = f12_read_full(f, wbuf, manifest[i].size);
        ASSERT_EQ(f12_close(f), F12_OK);
        ASSERT_EQ(t, manifest[i].size);
        ASSERT_EQ(checksum_buf(wbuf, t), manifest[i].checksum);
        pass++;
    }
    printf("  NEW files: %d/%d verified\n", pass, mc);

    int surv = 0;
    for (int i = 1; i < 50; i += 2) {
        char name[13];
        snprintf(name, sizeof(name), "FILL%02d.DAT", i);
        f12_file_t *f = f12_open(&fs, name, "r");
        if (!f) continue;
        uint32_t sz = 5000 + i * 200;
        uint32_t t = f12_read_full(f, wbuf, sz);
        ASSERT_EQ(f12_close(f), F12_OK);
        if (t != sz) continue;
        bool ok = true;
        for (uint32_t j = 0; j < sz; j++) {
            if (wbuf[j] != gen_pattern_byte(i + 1000, j)) { ok = false; break; }
        }
        if (ok) surv++;
    }
    printf("  Surviving FILL files: %d/25 verified\n", surv);
    ASSERT_EQ(surv, 25);

    for (int i = 0; i < 50; i += 2) {
        char name[13];
        snprintf(name, sizeof(name), "FILL%02d.DAT", i);
        ASSERT_EQ(f12_stat(&fs, name, &stat), F12_ERR_NOT_FOUND);
    }
    printf("  Deleted FILL files: confirmed gone\n  ");

    free(wbuf);
    f12_unmount(&fs);
}

static uint32_t fuzz_seed;

static uint32_t fuzz_rand(void) {
    fuzz_seed = fuzz_seed * 1103515245 + 12345;
    return (fuzz_seed >> 16) & 0x7FFF;
}

static void fuzz_rand_name(char *out) {
    int name_len = 1 + fuzz_rand() % 8;
    int ext_len = fuzz_rand() % 4;
    for (int i = 0; i < name_len; i++) {
        out[i] = 'A' + fuzz_rand() % 26;
    }
    if (ext_len > 0) {
        out[name_len] = '.';
        for (int i = 0; i < ext_len; i++) {
            out[name_len + 1 + i] = 'A' + fuzz_rand() % 26;
        }
        out[name_len + 1 + ext_len] = '\0';
    } else {
        out[name_len] = '\0';
    }
}

static void decode_image_from_scp_buf(uint8_t *scp_data, size_t scp_size,
                                       uint8_t image[2880][512]) {
    flux_sim_t sim;
    flux_sim_open_scp(&sim, scp_data, scp_size);
    for (int track = 0; track < 80; track++) {
        for (int side = 0; side < 2; side++) {
            if (!flux_sim_seek(&sim, track, side, 0)) continue;
            mfm_t mfm;
            mfm_init(&mfm);
            sector_t out;
            uint16_t delta;
            while (flux_sim_next(&sim, &delta)) {
                if (mfm_feed(&mfm, delta, &out)) {
                    if (out.valid && out.track == track && out.side == side &&
                        out.sector_n >= 1 && out.sector_n <= SECTORS_PER_TRACK) {
                        int lba = (track * 2 + side) * SECTORS_PER_TRACK + (out.sector_n - 1);
                        memcpy(image[lba], out.data, 512);
                    }
                }
            }
        }
    }
    flux_sim_close(&sim);
}

TEST(test_fsck_flux_roundtrip) {
    memcpy(disk_modified, disk_original, sizeof(disk_original));
    image_to_vdisk(disk_modified, &shared_vdisk);

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    ASSERT_EQ(f12_mount(&fs, make_vdisk_io(&shared_vdisk)), F12_OK);
    printf("\n");

    fat12_fsck_t report;
    ASSERT_EQ(f12_fsck(&fs, &report, false), F12_OK);
    printf("  original 1994 disk: files=%u dirs=%u lost=%u broken=%u cross=%u mismatch=%d\n",
           report.files, report.directories, report.lost_clusters,
           report.broken_chains, report.crosslinked, report.fat_mismatch);
    ASSERT_EQ(report.lost_clusters, 0);
    ASSERT_EQ(report.broken_chains, 0);
    ASSERT_EQ(report.crosslinked, 0);
    ASSERT(!report.fat_mismatch);
    uint16_t orig_files = report.files;

    uint8_t *rbuf = malloc(65536);
    file_manifest_t manifest[MAX_MANIFEST];
    int mc = 0;

    f12_dir_t dir;
    f12_stat_t stat;
    f12_opendir(&fs, "/", &dir);
    while (f12_readdir(&dir, &stat) == F12_OK && mc < MAX_MANIFEST) {
        if (stat.is_dir) continue;
        memset(manifest[mc].name, 0, 13);
        strncpy(manifest[mc].name, stat.name, 12);
        manifest[mc].size = stat.size;

        f12_file_t *f = f12_open(&fs, stat.name, "r");
        ASSERT(f != NULL);
        uint32_t ck = 0;
        int n;
        while ((n = f12_read(f, rbuf, 65536)) > 0) {
            for (int i = 0; i < n; i++) ck = (ck << 5) + ck + rbuf[i];
        }
        ASSERT_EQ(f12_close(f), F12_OK);
        manifest[mc].checksum = ck;
        mc++;
    }
    f12_closedir(&dir);
    ASSERT(mc > 0);
    printf("  %d root files in manifest (plus %u files in %u subdirectories)\n",
           mc, (unsigned)(orig_files - mc), report.directories);

    int del = -1;
    for (int m = 0; m < mc; m++) {
        if (manifest[m].size >= 35000 && strcmp(manifest[m].name, "LHA.DOC") != 0) {
            del = m;
            break;
        }
    }
    ASSERT(del >= 0);
    char deleted_name[13];
    memcpy(deleted_name, manifest[del].name, 13);
    ASSERT_EQ(f12_delete(&fs, deleted_name), F12_OK);
    printf("  deleted %s (%u bytes) to make room for the ghost write\n",
           deleted_name, manifest[del].size);
    manifest[del] = manifest[mc - 1];
    mc--;

    uint16_t free_orig = 0;
    ASSERT_EQ(fat12_free_count(&fs.fat, &free_orig), FAT12_OK);

    fat12_dirent_t lha;
    ASSERT_EQ(fat12_find(&fs.fat, "LHA.DOC", &lha), FAT12_OK);
    uint16_t lha_second = 0;
    ASSERT_EQ(fat12_get_entry(&fs.fat, lha.start_cluster, &lha_second), FAT12_OK);
    ASSERT(lha_second >= 2);
    uint16_t lha_clusters = (lha.size + 511) / 512;
    ASSERT(lha_clusters > 2);

    f12_file_t *ghost = f12_open(&fs, "GHOST.BIN", "w");
    ASSERT(ghost != NULL);
    memset(rbuf, 0xEE, 30000);
    ASSERT_EQ(f12_write(ghost, rbuf, 30000), 30000);
    fat12_abort_write(&fs.fat);
    ghost->mode = F12_MODE_CLOSED;
    printf("  damage: power cut mid-write of GHOST.BIN (30000 bytes, never closed)\n");

    f12_unmount(&fs);

    vdisk_set_fat_entry(&shared_vdisk, lha_second, 0);
    shared_vdisk.data[VDISK_FAT2_START][7] ^= 0x5A;
    printf("  damage: LHA.DOC chain broken at cluster %u, FAT2 byte flipped\n", lha_second);

    memset(&fs, 0, sizeof(fs));
    ASSERT_EQ(f12_mount(&fs, make_vdisk_io(&shared_vdisk)), F12_OK);
    ASSERT(fs.fat.fat_mismatch);

    ASSERT_EQ(f12_fsck(&fs, &report, false), F12_OK);
    printf("  fsck check: %u lost, %u broken, %u crosslinked, mismatch=%d\n",
           report.lost_clusters, report.broken_chains, report.crosslinked,
           report.fat_mismatch);
    ASSERT(report.lost_clusters >= (uint16_t)(lha_clusters - 2));
    ASSERT_EQ(report.broken_chains, 1);
    ASSERT_EQ(report.crosslinked, 0);
    ASSERT(report.fat_mismatch);
    ASSERT_EQ(report.freed, 0);

    ASSERT_EQ(f12_fsck(&fs, &report, true), F12_OK);
    ASSERT_EQ(report.freed, report.lost_clusters);
    ASSERT(report.repaired_fat2);
    printf("  fsck fix: freed %u clusters, terminated 1 chain, rewrote FAT2\n",
           report.freed);

    ASSERT_EQ(f12_fsck(&fs, &report, false), F12_OK);
    ASSERT_EQ(report.lost_clusters, 0);
    ASSERT_EQ(report.broken_chains, 0);
    ASSERT(!report.fat_mismatch);

    f12_unmount(&fs);

    vdisk_to_image(&shared_vdisk, disk_modified);
    size_t scp_size;
    uint8_t *scp_data = scp_encode_disk(disk_modified, &scp_size);
    ASSERT(scp_data != NULL);
    decode_image_from_scp_buf(scp_data, scp_size, disk_roundtrip);
    free(scp_data);

    int match = 0;
    for (int i = 0; i < 2880; i++) {
        if (memcmp(disk_modified[i], disk_roundtrip[i], 512) == 0) match++;
    }
    ASSERT_EQ(match, 2880);
    printf("  repaired disk: 2880/2880 sectors survive flux roundtrip\n");

    image_to_vdisk(disk_roundtrip, &shared_vdisk);
    memset(&fs, 0, sizeof(fs));
    ASSERT_EQ(f12_mount(&fs, make_vdisk_io(&shared_vdisk)), F12_OK);
    ASSERT(!fs.fat.fat_mismatch);

    ASSERT_EQ(f12_fsck(&fs, &report, false), F12_OK);
    ASSERT_EQ(report.lost_clusters, 0);
    ASSERT_EQ(report.broken_chains, 0);
    ASSERT_EQ(report.crosslinked, 0);
    ASSERT(!report.fat_mismatch);
    ASSERT_EQ(report.files, (uint16_t)(orig_files - 1));

    uint16_t free_now = 0;
    ASSERT_EQ(fat12_free_count(&fs.fat, &free_now), FAT12_OK);
    ASSERT_EQ(free_now, (uint16_t)(free_orig + lha_clusters - 2));

    for (int m = 0; m < mc; m++) {
        f12_file_t *f = f12_open(&fs, manifest[m].name, "r");
        ASSERT(f != NULL);
        uint32_t total = 0;
        uint32_t ck = 0;
        int n;
        while ((n = f12_read(f, rbuf, 65536)) > 0) {
            for (int i = 0; i < n; i++) ck = (ck << 5) + ck + rbuf[i];
            total += (uint32_t)n;
        }
        ASSERT_EQ(f12_close(f), F12_OK);

        if (strcmp(manifest[m].name, "LHA.DOC") == 0) {
            ASSERT_EQ(total, 1024);
        } else {
            ASSERT_EQ(total, manifest[m].size);
            ASSERT_EQ(ck, manifest[m].checksum);
        }
    }
    printf("  %d files verified after repair + flux roundtrip (LHA.DOC truncated to 1024 as repaired)\n", mc);

    f12_stat_t ghost_stat;
    ASSERT_EQ(f12_stat(&fs, "GHOST.BIN", &ghost_stat), F12_ERR_NOT_FOUND);

    free(rbuf);
    f12_unmount(&fs);
}

TEST(test_fuzz_roundtrip) {
    int iterations = 100;
    uint8_t *wbuf = malloc(65536);

    printf("\n  Seed: %u\n", fuzz_seed);
    printf("  Iterations: %d\n\n", iterations);

    int total_files_created = 0;
    int total_files_deleted = 0;
    int total_files_renamed = 0;
    int total_files_verified = 0;
    int total_roundtrips = 0;

    for (int iter = 0; iter < iterations; iter++) {
        vdisk_init(&shared_vdisk);
        f12_t fs;
        memset(&fs, 0, sizeof(fs));
        fs.io = make_vdisk_io(&shared_vdisk);
        f12_format(&fs, "FUZZ", false);
        f12_mount(&fs, make_vdisk_io(&shared_vdisk));

        file_manifest_t manifest[MAX_MANIFEST];
        int mc = 0;

        int num_ops = 5 + fuzz_rand() % 40;
        for (int op = 0; op < num_ops && mc < MAX_MANIFEST - 1; op++) {
            int action = fuzz_rand() % 10;

            if (action < 6 || mc == 0) {
                char name[13];
                fuzz_rand_name(name);

                uint32_t sz = fuzz_rand() % 50000;

                f12_file_t *f = f12_open(&fs, name, "w");
                if (!f) continue;

                uint32_t written = 0;
                bool write_ok = true;
                while (written < sz) {
                    uint32_t chunk = sz - written;
                    if (chunk > 512) chunk = 512;
                    for (uint32_t j = 0; j < chunk; j++)
                        wbuf[j] = gen_pattern_byte(iter * 1000 + op, written + j);
                    int n = f12_write(f, wbuf, chunk);
                    if (n <= 0) { write_ok = false; break; }
                    written += n;
                }
                f12_err_t close_err = f12_close(f);
                if (write_ok) {
                    ASSERT_EQ(close_err, F12_OK);
                } else {
                    ASSERT(close_err == F12_OK || close_err == F12_ERR_FULL);
                }

                if (!write_ok || close_err != F12_OK || (written == 0 && sz > 0)) {
                    f12_delete_if_present(&fs, name);
                    continue;
                }

                bool replaced = false;
                for (int m = 0; m < mc; m++) {
                    if (strcmp(manifest[m].name, name) == 0) {
                        manifest[m].size = written;
                        uint32_t ck = 0;
                        for (uint32_t j = 0; j < written; j++)
                            ck = (ck << 5) + ck + gen_pattern_byte(iter * 1000 + op, j);
                        manifest[m].checksum = ck;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    memset(manifest[mc].name, 0, 13);
                    strncpy(manifest[mc].name, name, 12);
                    manifest[mc].size = written;
                    uint32_t ck = 0;
                    for (uint32_t j = 0; j < written; j++)
                        ck = (ck << 5) + ck + gen_pattern_byte(iter * 1000 + op, j);
                    manifest[mc].checksum = ck;
                    mc++;
                }
                total_files_created++;

            } else if (action < 8 && mc > 0) {
                int victim = fuzz_rand() % mc;
                f12_err_t err = f12_delete(&fs, manifest[victim].name);
                if (err == F12_OK) {
                    manifest[victim] = manifest[mc - 1];
                    mc--;
                    total_files_deleted++;
                }
            } else if (mc > 0) {
                int victim = fuzz_rand() % mc;
                char newname[13];
                fuzz_rand_name(newname);

                f12_err_t err = f12_rename(&fs, manifest[victim].name, newname);
                if (err == F12_OK) {
                    memset(manifest[victim].name, 0, 13);
                    strncpy(manifest[victim].name, newname, 12);
                    total_files_renamed++;
                } else {
                    ASSERT_EQ(err, F12_ERR_EXISTS);
                }
            }
        }

        f12_unmount(&fs);
        vdisk_to_image(&shared_vdisk, disk_modified);

        size_t scp_size;
        uint8_t *scp_data = scp_encode_disk(disk_modified, &scp_size);
        if (!scp_data) {
            printf("  iter %d: encode failed!\n", iter);
            exit(1);
        }

        decode_image_from_scp_buf(scp_data, scp_size, disk_roundtrip);
        free(scp_data);

        for (int i = 0; i < 2880; i++) {
            if (memcmp(disk_modified[i], disk_roundtrip[i], 512) != 0) {
                printf("  iter %d: sector %d mismatch after roundtrip!\n", iter, i);
                exit(1);
            }
        }

        image_to_vdisk(disk_roundtrip, &shared_vdisk);
        memset(&fs, 0, sizeof(fs));
        if (f12_mount(&fs, make_vdisk_io(&shared_vdisk)) != F12_OK) {
            printf("  iter %d: mount failed after roundtrip!\n", iter);
            exit(1);
        }

        for (int m = 0; m < mc; m++) {
            f12_stat_t stat;
            if (f12_stat(&fs, manifest[m].name, &stat) != F12_OK) {
                printf("  iter %d: file '%s' missing after roundtrip!\n",
                       iter, manifest[m].name);
                exit(1);
            }

            if (stat.size != manifest[m].size) {
                printf("  iter %d: file '%s' size %u != expected %u\n",
                       iter, manifest[m].name, stat.size, manifest[m].size);
                exit(1);
            }

            if (manifest[m].size > 0) {
                f12_file_t *f = f12_open(&fs, manifest[m].name, "r");
                if (!f) {
                    printf("  iter %d: can't open '%s' for read!\n", iter, manifest[m].name);
                    exit(1);
                }
                uint32_t t = f12_read_full(f, wbuf, manifest[m].size);
                ASSERT_EQ(f12_close(f), F12_OK);

                if (t != manifest[m].size) {
                    printf("  iter %d: file '%s' read %u != size %u\n",
                           iter, manifest[m].name, t, manifest[m].size);
                    exit(1);
                }

                if (checksum_buf(wbuf, t) != manifest[m].checksum) {
                    printf("  iter %d: file '%s' checksum mismatch!\n",
                           iter, manifest[m].name);
                    exit(1);
                }
            }
            total_files_verified++;
        }

        f12_unmount(&fs);
        total_roundtrips++;

        if ((iter + 1) % 100 == 0) {
            printf("  [%d/%d] roundtrips OK, %d created, %d deleted, %d verified\n",
                   iter + 1, iterations,
                   total_files_created, total_files_deleted, total_files_verified);
        }
    }

    printf("\n  === Fuzz Summary ===\n");
    printf("  Roundtrips:      %d\n", total_roundtrips);
    printf("  Files created:   %d\n", total_files_created);
    printf("  Files deleted:   %d\n", total_files_deleted);
    printf("  Files renamed:   %d\n", total_files_renamed);
    printf("  Files verified:  %d\n", total_files_verified);
    printf("  Seed: %u\n  ", fuzz_seed);

    free(wbuf);
    ASSERT_EQ(total_roundtrips, iterations);
}

int main(int argc, char *argv[]) {
    const char *scp_path = SCP_PATH;
    if (argc > 1) scp_path = argv[1];
    (void)scp_path;

    fuzz_seed = (uint32_t)time(NULL);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            fuzz_seed = (uint32_t)atoi(argv[++i]);
        }
    }

    printf("=== SCP Full Roundtrip: decode -> f12 modify -> encode -> decode -> f12 verify ===\n\n");

    printf("--- Phase 1: Decode ---\n");
    RUN_TEST(test_decode_original);

    printf("\n--- Phase 2: Heavy F12 Modifications ---\n");
    RUN_TEST(test_f12_heavy_modifications);

    printf("\n--- Phase 3: MFM Encode to SCP ---\n");
    RUN_TEST(test_encode_to_scp);

    printf("\n--- Phase 4: MFM Decode from SCP ---\n");
    RUN_TEST(test_decode_roundtrip);

    printf("\n--- Phase 5: F12 Verify Everything ---\n");
    RUN_TEST(test_verify_all_after_roundtrip);

    printf("\n--- Phase 6: Format Fresh + Fill to Maximum ---\n");
    RUN_TEST(test_format_fill_max_roundtrip);

    printf("\n--- Phase 7: Format + Fill + Delete + Refill ---\n");
    RUN_TEST(test_format_fill_delete_refill_roundtrip);

    RUN_TEST(test_fsck_flux_roundtrip);

    printf("\n--- Phase 8: Fuzz Roundtrip (100 iterations) ---\n");
    RUN_TEST(test_fuzz_roundtrip);

    TEST_RESULTS();
}
