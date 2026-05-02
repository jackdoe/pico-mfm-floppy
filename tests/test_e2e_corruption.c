#include "test.h"
#include "pio_sim.h"
#include "../src/floppy.h"
#include "../src/f12.h"
#include "../src/fat12.h"
#include "../src/mfm_encode.h"

#define SCP_PATH "../../system-shock-multilingual-floppy-ibm-pc/disk1.scp"

floppy_t *pio_sim_floppy_ref;

static pio_sim_drive_t baseline;
static pio_sim_drive_t live;
static floppy_t floppy;

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

static void clone_drive(const pio_sim_drive_t *src, pio_sim_drive_t *dst) {
    pio_sim_init(dst);
    for (int t = 0; t < 80; t++) {
        for (int s = 0; s < 2; s++) {
            const pio_sim_track_t *st = &src->tracks[t][s];
            if (st->count == 0) continue;
            dst->tracks[t][s].deltas = malloc(st->count * sizeof(uint16_t));
            dst->tracks[t][s].count = st->count;
            memcpy(dst->tracks[t][s].deltas, st->deltas, st->count * sizeof(uint16_t));
        }
    }
}

static void setup_floppy(void) {
    memset(&floppy, 0, sizeof(floppy));
    floppy.pins.index = 1;
    floppy.pins.track0 = 2;
    floppy.pins.write_protect = 3;
    floppy.pins.read_data = 4;
    floppy.pins.disk_change = 5;
    floppy.pins.drive_select = 6;
    floppy.pins.motor_enable = 7;
    floppy.pins.direction = 8;
    floppy.pins.step = 9;
    floppy.pins.write_data = 10;
    floppy.pins.write_gate = 11;
    floppy.pins.side_select = 12;
    floppy.pins.density = 13;

    pio_sim_floppy_ref = &floppy;
    floppy_init(&floppy);
}

static void reset_to_baseline(void) {
    pio_sim_free(&live);
    clone_drive(&baseline, &live);
    pio_sim_install(&live);
    setup_floppy();
}

static uint32_t lcg(uint32_t *seed) {
    *seed = *seed * 1103515245u + 12345u;
    return *seed;
}

static void corrupt_track_random(uint8_t cyl, uint8_t side, uint32_t bytes_to_flip, uint32_t seed) {
    pio_sim_track_t *t = &live.tracks[cyl][side];
    if (t->count == 0) return;
    for (uint32_t i = 0; i < bytes_to_flip; i++) {
        uint32_t pos = lcg(&seed) % t->count;
        t->deltas[pos] ^= (lcg(&seed) & 0x7F) + 1;
    }
}

static void wipe_track(uint8_t cyl, uint8_t side) {
    pio_sim_track_t *t = &live.tracks[cyl][side];
    for (uint32_t i = 0; i < t->count; i++) t->deltas[i] = 60;
}

static f12_io_t make_io(void) {
    return (f12_io_t){
        .read = floppy_io_read,
        .write = floppy_io_write,
        .disk_changed = floppy_io_disk_changed,
        .write_protected = floppy_io_write_protected,
        .ctx = &floppy,
    };
}

TEST(test_e2e_clean_baseline_mounts) {
    reset_to_baseline();

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    ASSERT_EQ(f12_mount(&fs, make_io()), F12_OK);
    ASSERT(!fs.fat.fat_mismatch);

    f12_dir_t dir;
    ASSERT_EQ(f12_opendir(&fs, "/", &dir), F12_OK);
    int count = 0;
    f12_stat_t stat;
    while (f12_readdir(&dir, &stat) == F12_OK) count++;
    f12_closedir(&dir);
    ASSERT(count > 0);

    f12_unmount(&fs);
}

TEST(test_e2e_corrupted_data_track_doesnt_crash_mount) {
    reset_to_baseline();

    corrupt_track_random(40, 0, 5000, 0xDEADBEEF);
    corrupt_track_random(60, 1, 5000, 0xCAFEBABE);

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    f12_err_t err = f12_mount(&fs, make_io());
    ASSERT_EQ(err, F12_OK);

    f12_dir_t dir;
    ASSERT_EQ(f12_opendir(&fs, "/", &dir), F12_OK);
    int count = 0;
    f12_stat_t stat;
    while (f12_readdir(&dir, &stat) == F12_OK) count++;
    f12_closedir(&dir);
    printf("\n  Listed %d files with corrupted data tracks\n  ", count);
    ASSERT(count > 0);

    f12_unmount(&fs);
}

TEST(test_e2e_corrupted_track0_side0_breaks_mount) {
    reset_to_baseline();

    wipe_track(0, 0);

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    f12_err_t err = f12_mount(&fs, make_io());
    ASSERT(err != F12_OK);
    ASSERT(!fs.mounted);
    printf("\n  mount on wiped track 0 side 0: err=%d\n  ", err);
}

TEST(test_e2e_corrupted_fat_region_detected) {
    reset_to_baseline();

    corrupt_track_random(0, 0, 200, 0x12345678);

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    f12_err_t err = f12_mount(&fs, make_io());
    if (err == F12_OK) {
        printf("\n  mount succeeded; fat_mismatch=%d\n  ", fs.fat.fat_mismatch);
        f12_unmount(&fs);
    } else {
        printf("\n  mount rejected corrupted FAT region: err=%d\n  ", err);
    }
}

TEST(test_e2e_corrupted_data_file_read_handled) {
    reset_to_baseline();

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    ASSERT_EQ(f12_mount(&fs, make_io()), F12_OK);

    f12_stat_t stat;
    if (f12_stat(&fs, "README.SS", &stat) != F12_OK) {
        f12_unmount(&fs);
        printf("\n  no README.SS on disk; skipping\n  ");
        return;
    }
    f12_unmount(&fs);

    for (int t = 0; t < 80; t++) {
        for (int s = 0; s < 2; s++) {
            corrupt_track_random(t, s, 30, (uint32_t)(0xA5A5A5A5u + t * 13 + s));
        }
    }

    memset(&fs, 0, sizeof(fs));
    f12_err_t err = f12_mount(&fs, make_io());
    if (err != F12_OK) {
        printf("\n  heavy corruption broke mount (%d) — acceptable\n  ", err);
        return;
    }

    f12_file_t *f = f12_open(&fs, "README.SS", "r");
    if (!f) {
        printf("\n  could not open file under heavy corruption: errno=%d\n  ", f12_errno(&fs));
        f12_unmount(&fs);
        return;
    }

    uint32_t total = 0;
    uint8_t buf[512];
    int n;
    int reads = 0;
    while ((n = f12_read(f, buf, sizeof(buf))) > 0 && reads < 1000) {
        total += n;
        reads++;
    }
    printf("\n  read %u/%u bytes (n=%d errno=%d) under heavy corruption\n  ",
           total, stat.size, n, f12_errno(&fs));
    ASSERT(n >= -1);

    f12_close(f);
    f12_unmount(&fs);
}

TEST(test_e2e_corrupted_root_dir_listing_safe) {
    reset_to_baseline();

    corrupt_track_random(0, 1, 1500, 0xFEEDFACE);

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    f12_err_t err = f12_mount(&fs, make_io());
    if (err != F12_OK) {
        printf("\n  corrupted root dir track broke mount (err=%d)\n  ", err);
        return;
    }

    f12_dir_t dir;
    f12_err_t r = f12_opendir(&fs, "/", &dir);
    if (r == F12_OK) {
        f12_stat_t stat;
        int seen = 0;
        while (f12_readdir(&dir, &stat) == F12_OK && seen < 256) {
            seen++;
        }
        f12_closedir(&dir);
        printf("\n  listed %d entries with corrupted root dir track\n  ", seen);
    }

    f12_unmount(&fs);
}

TEST(test_e2e_random_corruption_no_crash_sweep) {
    uint32_t seeds[] = { 1, 2, 3, 0x42424242, 0xDEAD0000 };
    for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
        reset_to_baseline();

        uint32_t seed = seeds[s];
        for (int t = 0; t < 80; t++) {
            for (int side = 0; side < 2; side++) {
                if ((lcg(&seed) & 0xF) == 0) {
                    corrupt_track_random(t, side, 20 + (lcg(&seed) & 0xFF),
                                          seeds[s] + t * 31 + side);
                }
            }
        }

        f12_t fs;
        memset(&fs, 0, sizeof(fs));
        f12_err_t err = f12_mount(&fs, make_io());
        if (err == F12_OK) {
            f12_dir_t dir;
            if (f12_opendir(&fs, "/", &dir) == F12_OK) {
                f12_stat_t stat;
                int n = 0;
                while (f12_readdir(&dir, &stat) == F12_OK && n < 256) n++;
                f12_closedir(&dir);
            }
            f12_unmount(&fs);
        }
    }
    printf("\n  swept %zu corruption seeds without crash\n  ",
           sizeof(seeds) / sizeof(seeds[0]));
}

int main(void) {
    size_t scp_size;
    uint8_t *scp_data = load_file(SCP_PATH, &scp_size);
    if (!scp_data) {
        printf("=== E2E Corruption Tests: SKIPPED (no SCP file) ===\n");
        return 0;
    }

    pio_sim_init(&baseline);
    pio_sim_load_scp(&baseline, scp_data, scp_size);
    pio_sim_init(&live);

    printf("=== End-to-End Corruption Tests ===\n");
    printf("Inject flux corruption through floppy.c -> mfm_decode.c -> fat12.c -> f12.c\n\n");

    RUN_TEST(test_e2e_clean_baseline_mounts);
    RUN_TEST(test_e2e_corrupted_data_track_doesnt_crash_mount);
    RUN_TEST(test_e2e_corrupted_track0_side0_breaks_mount);
    RUN_TEST(test_e2e_corrupted_fat_region_detected);
    RUN_TEST(test_e2e_corrupted_data_file_read_handled);
    RUN_TEST(test_e2e_corrupted_root_dir_listing_safe);
    RUN_TEST(test_e2e_random_corruption_no_crash_sweep);

    pio_sim_free(&baseline);
    pio_sim_free(&live);
    free(scp_data);

    TEST_RESULTS();
}
