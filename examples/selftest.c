#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "floppy.h"
#include "f12.h"

#define NUM_TEST_FILES 10
#define LED_PIN 25

static uint32_t checksum_buf(const uint8_t *buf, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (sum << 5) + sum + buf[i];
    }
    return sum;
}

static uint32_t gen_pattern_byte(int file_id, uint32_t offset) {
    uint32_t v = file_id * 2654435761u + offset * 40503u;
    return (v >> 16) & 0xFF;
}

static void fill_pattern(uint8_t *buf, int file_id, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        buf[i] = gen_pattern_byte(file_id, i);
    }
}

static uint32_t pattern_checksum(int file_id, uint32_t size) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < size; i++) {
        sum = (sum << 5) + sum + gen_pattern_byte(file_id, i);
    }
    return sum;
}

typedef struct {
    char name[13];
    uint32_t size;
    uint32_t checksum;
} test_file_t;

static test_file_t test_files[NUM_TEST_FILES] = {
    {"TINY.BIN",   1,     0},
    {"SMALL.DAT",  100,   0},
    {"HALF.DAT",   256,   0},
    {"SECT.DAT",   512,   0},
    {"MULTI.DAT",  1024,  0},
    {"MED.DAT",    4096,  0},
    {"BIG.DAT",    10000, 0},
    {"LARGE.DAT",  20000, 0},
    {"HUGE.DAT",   35000, 0},
    {"MAX.DAT",    50000, 0},
};

static int pass_count;
static int fail_count;

static void check(bool cond, const char *tag) {
    if (cond) {
        printf("  PASS: %s\n", tag);
        pass_count++;
    } else {
        printf("  FAIL: %s\n", tag);
        fail_count++;
    }
}

static f12_err_t do_mount(f12_t *fs, floppy_t *floppy) {
    return f12_mount(fs, (f12_io_t){
        .read = floppy_io_read,
        .write = floppy_io_write,
        .disk_changed = floppy_io_disk_changed,
        .write_protected = floppy_io_write_protected,
        .ctx = floppy,
    });
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

int main(void) {
    stdio_init_all();
    sleep_ms(3000);

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    printf("\n\n========================================\n");
    printf("  FLOPPY SELF-TEST\n");
    printf("========================================\n\n");

    floppy_t floppy = {
        .pins = {
            .index         = 2,
            .track0        = 3,
            .write_protect = 4,
            .read_data     = 5,
            .disk_change   = 6,
            .drive_select  = 7,
            .motor_enable  = 8,
            .direction     = 9,
            .step          = 10,
            .write_data    = 11,
            .write_gate    = 12,
            .side_select   = 13,
            .density       = 14,
        }
    };

    floppy_init(&floppy);
    floppy_set_density(&floppy, true);

    printf("[INIT] Floppy initialized, HD mode\n");
    printf("[INIT] Write protected: %s\n", floppy_write_protected(&floppy) ? "YES" : "no");

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    pass_count = 0;
    fail_count = 0;

    printf("\n--- Phase 1: Mount Existing Disk ---\n");
    f12_err_t err = do_mount(&fs, &floppy);
    if (err == F12_OK) {
        printf("  Existing disk mounted, listing files:\n");
        f12_dir_t dir;
        f12_stat_t stat;
        f12_opendir(&fs, "/", &dir);
        int count = 0;
        while (f12_readdir(&dir, &stat) == F12_OK) {
            printf("    %-12s %8lu\n", stat.name, stat.size);
            count++;
        }
        f12_closedir(&dir);
        printf("  %d files found\n", count);
        f12_unmount(&fs);
    } else {
        printf("  No existing filesystem (%s)\n", f12_strerror(err));
    }

    printf("\n--- Phase 2: Format ---\n");
    fs.io = (f12_io_t){
        .read = floppy_io_read,
        .write = floppy_io_write,
        .disk_changed = floppy_io_disk_changed,
        .write_protected = floppy_io_write_protected,
        .ctx = &floppy,
    };
    err = f12_format(&fs, "SELFTEST", false);
    check(err == F12_OK, "format quick");

    err = do_mount(&fs, &floppy);
    check(err == F12_OK, "mount after format");

    printf("\n--- Phase 3: Write %d Test Files ---\n", NUM_TEST_FILES);
    static uint8_t wbuf[50000];

    for (int i = 0; i < NUM_TEST_FILES; i++) {
        test_files[i].checksum = pattern_checksum(i, test_files[i].size);
    }

    for (int i = 0; i < NUM_TEST_FILES; i++) {
        fill_pattern(wbuf, i, test_files[i].size);
        f12_file_t *f = f12_open(&fs, test_files[i].name, "w");
        if (!f) {
            printf("  FAIL: open %s for write: %s\n", test_files[i].name, f12_strerror(f12_errno(&fs)));
            fail_count++;
            continue;
        }
        f12_write_full(f, wbuf, test_files[i].size);
        f12_close(f);
        printf("  wrote %s (%lu bytes)\n", test_files[i].name, test_files[i].size);
    }

    printf("\n--- Phase 4: Read Back & Verify ---\n");
    for (int i = 0; i < NUM_TEST_FILES; i++) {
        f12_file_t *f = f12_open(&fs, test_files[i].name, "r");
        if (!f) {
            printf("  FAIL: open %s for read\n", test_files[i].name);
            fail_count++;
            continue;
        }
        uint32_t got = f12_read_full(f, wbuf, test_files[i].size);
        f12_close(f);

        f12_stat_t stat;
        f12_stat(&fs, test_files[i].name, &stat);

        bool size_ok = (got == test_files[i].size && stat.size == test_files[i].size);
        bool cksum_ok = (checksum_buf(wbuf, got) == test_files[i].checksum);

        char tag[64];
        snprintf(tag, sizeof(tag), "%s size=%lu cksum=0x%08lX",
                 test_files[i].name, got, checksum_buf(wbuf, got));
        check(size_ok && cksum_ok, tag);
    }

    printf("\n--- Phase 5: Delete 5 Files ---\n");
    for (int i = 0; i < 5; i++) {
        err = f12_delete(&fs, test_files[i].name);
        char tag[32];
        snprintf(tag, sizeof(tag), "delete %s", test_files[i].name);
        check(err == F12_OK, tag);
    }

    for (int i = 0; i < 5; i++) {
        f12_stat_t stat;
        err = f12_stat(&fs, test_files[i].name, &stat);
        char tag[32];
        snprintf(tag, sizeof(tag), "%s gone", test_files[i].name);
        check(err == F12_ERR_NOT_FOUND, tag);
    }

    printf("\n--- Phase 6: Write 5 New Files in Freed Space ---\n");
    test_file_t new_files[5] = {
        {"NEW01.DAT", 500,   0},
        {"NEW02.DAT", 2048,  0},
        {"NEW03.DAT", 8000,  0},
        {"NEW04.DAT", 15000, 0},
        {"NEW05.DAT", 30000, 0},
    };

    for (int i = 0; i < 5; i++) {
        new_files[i].checksum = pattern_checksum(100 + i, new_files[i].size);
        fill_pattern(wbuf, 100 + i, new_files[i].size);
        f12_file_t *f = f12_open(&fs, new_files[i].name, "w");
        if (!f) {
            printf("  FAIL: open %s for write\n", new_files[i].name);
            fail_count++;
            continue;
        }
        f12_write_full(f, wbuf, new_files[i].size);
        f12_close(f);
        printf("  wrote %s (%lu bytes)\n", new_files[i].name, new_files[i].size);
    }

    printf("\n--- Phase 7: Verify ALL Remaining Files ---\n");
    for (int i = 5; i < NUM_TEST_FILES; i++) {
        f12_file_t *f = f12_open(&fs, test_files[i].name, "r");
        if (!f) {
            printf("  FAIL: open %s\n", test_files[i].name);
            fail_count++;
            continue;
        }
        uint32_t got = f12_read_full(f, wbuf, test_files[i].size);
        f12_close(f);
        bool ok = (got == test_files[i].size) &&
                  (checksum_buf(wbuf, got) == test_files[i].checksum);
        char tag[64];
        snprintf(tag, sizeof(tag), "original %s verified", test_files[i].name);
        check(ok, tag);
    }

    for (int i = 0; i < 5; i++) {
        f12_file_t *f = f12_open(&fs, new_files[i].name, "r");
        if (!f) {
            printf("  FAIL: open %s\n", new_files[i].name);
            fail_count++;
            continue;
        }
        uint32_t got = f12_read_full(f, wbuf, new_files[i].size);
        f12_close(f);
        bool ok = (got == new_files[i].size) &&
                  (checksum_buf(wbuf, got) == new_files[i].checksum);
        char tag[64];
        snprintf(tag, sizeof(tag), "new %s verified", new_files[i].name);
        check(ok, tag);
    }

    printf("\n--- Phase 8: Read All 2880 Sectors ---\n");
    int valid_sectors = 0;
    int invalid_sectors = 0;
    sector_t sector;

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
            if (track_valid < SECTORS_PER_TRACK) {
                printf("  T%02d/S%d: %d/%d sectors\n", track, side,
                       track_valid, SECTORS_PER_TRACK);
            }
        }
        if ((track + 1) % 10 == 0) {
            printf("  ... %d tracks done\n", track + 1);
        }
    }
    printf("  Valid: %d  Invalid: %d  Total: %d\n",
           valid_sectors, invalid_sectors, valid_sectors + invalid_sectors);
    check(valid_sectors == 2880, "all 2880 sectors readable");

    f12_unmount(&fs);

    printf("\n========================================\n");
    printf("  RESULTS: %d passed, %d failed\n", pass_count, fail_count);
    printf("  %s\n", fail_count == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    printf("========================================\n\n");

    for (;;) {
        gpio_put(LED_PIN, 1);
        sleep_ms(fail_count == 0 ? 500 : 100);
        gpio_put(LED_PIN, 0);
        sleep_ms(fail_count == 0 ? 500 : 100);
    }

    return 0;
}
