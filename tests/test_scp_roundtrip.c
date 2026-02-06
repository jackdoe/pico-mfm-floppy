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
    f12_close(f);

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
    f12_close(f);
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
    f12_close(f);
    printf("  [CREATE] HELLO.TXT (%zu bytes)\n", strlen(hello));

    f12_stat(&fs, "HELLO.TXT", &stat);
    ASSERT_EQ(stat.size, (uint32_t)strlen(hello));

    f = f12_open(&fs, "BIG.DAT", "w");
    ASSERT(f != NULL);
    uint8_t pattern[10000];
    for (int i = 0; i < 10000; i++) pattern[i] = (i * 7 + 13) & 0xFF;
    f12_write_full(f, pattern, sizeof(pattern));
    f12_close(f);
    printf("  [CREATE] BIG.DAT (10000 bytes)\n");

    f12_stat(&fs, "BIG.DAT", &stat);
    ASSERT_EQ(stat.size, 10000);

    f = f12_open(&fs, "TINY.BIN", "w");
    ASSERT(f != NULL);
    uint8_t one_byte = 0x42;
    n = f12_write(f, &one_byte, 1);
    ASSERT_EQ(n, 1);
    f12_close(f);
    printf("  [CREATE] TINY.BIN (1 byte)\n");

    f = f12_open(&fs, "EMPTY.TXT", "w");
    ASSERT(f != NULL);
    f12_close(f);
    printf("  [CREATE] EMPTY.TXT (0 bytes)\n");

    f = f12_open(&fs, "CYB.CFG", "w");
    ASSERT(f != NULL);
    const char *new_cfg = "overwritten_config_data_here_1234567890";
    f12_write_full(f, new_cfg, strlen(new_cfg));
    f12_close(f);
    printf("  [OVERWRITE] CYB.CFG: 54 -> %zu bytes\n", strlen(new_cfg));

    f12_stat(&fs, "CYB.CFG", &stat);
    ASSERT_EQ(stat.size, (uint32_t)strlen(new_cfg));

    printf("\n  --- Verify all modifications in vdisk ---\n");

    f = f12_open(&fs, "README.SS", "r");
    ASSERT(f != NULL);
    uint8_t *check = malloc(readme_size + 1);
    uint32_t t = f12_read_full(f, check, readme_size);
    f12_close(f);
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

    f = f12_open(&fs, "HELLO.TXT", "r");
    ASSERT(f != NULL);
    char hello_buf[256] = {0};
    t = f12_read_full(f, hello_buf, sizeof(hello_buf));
    f12_close(f);
    ASSERT_EQ(t, (uint32_t)strlen(hello));
    ASSERT(memcmp(hello_buf, hello, strlen(hello)) == 0);
    printf("  HELLO.TXT: content verified\n");

    f = f12_open(&fs, "BIG.DAT", "r");
    ASSERT(f != NULL);
    uint8_t *big_buf = malloc(10000);
    t = f12_read_full(f, big_buf, 10000);
    f12_close(f);
    ASSERT_EQ(t, 10000);
    ASSERT_MEM_EQ(big_buf, pattern, 10000);
    printf("  BIG.DAT: 10000 bytes verified\n");
    free(big_buf);

    f = f12_open(&fs, "TINY.BIN", "r");
    ASSERT(f != NULL);
    uint8_t tiny_buf;
    n = f12_read(f, &tiny_buf, 1);
    f12_close(f);
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
    f12_close(f);
    ASSERT_EQ(t, (uint32_t)strlen(new_cfg));
    ASSERT(memcmp(cfg_buf, new_cfg, strlen(new_cfg)) == 0);
    printf("  CYB.CFG: overwritten content verified\n");

    f = f12_open(&fs, "INSTALL.EXE", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "INSTALL.EXE", &stat);
    uint8_t *install_buf = malloc(stat.size);
    t = f12_read_full(f, install_buf, stat.size);
    f12_close(f);
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
    f12_close(f);
    readme[t] = '\0';
    char *found = strstr(readme, "Floppy");
    ASSERT(found != NULL);
    printf("  README.SS: 'Floppy' found at offset %ld\n", found - readme);
    free(readme);

    ASSERT_EQ(f12_stat(&fs, "LHA.DOC", &stat), F12_ERR_NOT_FOUND);
    printf("  LHA.DOC: confirmed deleted\n");

    ASSERT_EQ(f12_stat(&fs, "SHOCKGUS.BAT", &stat), F12_ERR_NOT_FOUND);
    printf("  SHOCKGUS.BAT: confirmed deleted\n");

    f = f12_open(&fs, "HELLO.TXT", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "HELLO.TXT", &stat);
    char *hello = malloc(stat.size + 1);
    t = f12_read_full(f, hello, stat.size);
    f12_close(f);
    hello[t] = '\0';
    ASSERT(strstr(hello, "floppy controller") != NULL);
    printf("  HELLO.TXT: content survived (%u bytes)\n", t);
    free(hello);

    f = f12_open(&fs, "BIG.DAT", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "BIG.DAT", &stat);
    ASSERT_EQ(stat.size, 10000);
    uint8_t *big = malloc(10000);
    t = f12_read_full(f, big, 10000);
    f12_close(f);
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
    f12_close(f);
    printf("  TINY.BIN: 0x42 verified\n");

    f12_stat(&fs, "EMPTY.TXT", &stat);
    ASSERT_EQ(stat.size, 0);
    printf("  EMPTY.TXT: 0 bytes confirmed\n");

    f = f12_open(&fs, "CYB.CFG", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "CYB.CFG", &stat);
    char *cfg = malloc(stat.size + 1);
    t = f12_read_full(f, cfg, stat.size);
    f12_close(f);
    cfg[t] = '\0';
    ASSERT(strstr(cfg, "overwritten_config") != NULL);
    printf("  CYB.CFG: overwritten content survived (%u bytes)\n", t);
    free(cfg);

    f = f12_open(&fs, "INSTALL.EXE", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "INSTALL.EXE", &stat);
    uint8_t *exe = malloc(stat.size);
    t = f12_read_full(f, exe, stat.size);
    f12_close(f);
    ASSERT_EQ(checksum_buf(exe, t), 0xD3FA0E4E);
    printf("  INSTALL.EXE: checksum 0xD3FA0E4E (untouched)\n");
    free(exe);

    f = f12_open(&fs, "BASE.LZH", "r");
    ASSERT(f != NULL);
    f12_stat(&fs, "BASE.LZH", &stat);
    uint8_t *lzh = malloc(stat.size);
    t = f12_read_full(f, lzh, stat.size);
    f12_close(f);
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

    uint8_t *wbuf = malloc(65536);

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
        f12_close(f);

        if (!write_ok || written == 0) {
            f12_delete(&fs, name);
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
        f12_close(f);
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
        f12_close(f);
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
        f12_close(f);
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
        while (written < sz) {
            uint32_t chunk = sz - written;
            if (chunk > 512) chunk = 512;
            int n = f12_write(f, wbuf + written, chunk);
            if (n <= 0) break;
            written += n;
        }
        f12_close(f);
        if (written == 0) break;

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
        f12_close(f);
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
        f12_close(f);
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

int main(int argc, char *argv[]) {
    const char *scp_path = SCP_PATH;
    if (argc > 1) scp_path = argv[1];
    (void)scp_path;

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

    TEST_RESULTS();
}
