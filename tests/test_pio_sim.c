#include "test.h"
#include "pio_sim.h"
#include "../src/floppy.h"
#include "../src/mfm_encode.h"
#include "../src/f12.h"

#define SCP_PATH "../../system-shock-multilingual-floppy-ibm-pc/disk1.scp"

floppy_t *pio_sim_floppy_ref;

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

static pio_sim_drive_t sim_drive;
static floppy_t floppy;

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

TEST(test_pio_read_sector) {
    setup_floppy();

    sector_t sector;
    sector.track = 0;
    sector.side = 0;
    sector.sector_n = 1;
    sector.valid = false;

    floppy_status_t status = floppy_read_sector(&floppy, &sector);

    printf("\n  floppy_read_sector(0,0,1): status=%d, valid=%d\n",
           status, sector.valid);

    ASSERT_EQ(status, FLOPPY_OK);
    ASSERT(sector.valid);

    ASSERT_EQ(sector.data[510], 0x55);
    ASSERT_EQ(sector.data[511], 0xAA);
    printf("  Boot sector signature: %02X%02X\n  ", sector.data[510], sector.data[511]);
}

TEST(test_pio_read_all_track0) {
    setup_floppy();

    int found = 0;
    for (int s = 1; s <= SECTORS_PER_TRACK; s++) {
        sector_t sector;
        sector.track = 0;
        sector.side = 0;
        sector.sector_n = s;
        sector.valid = false;

        floppy_status_t status = floppy_read_sector(&floppy, &sector);
        if (status == FLOPPY_OK && sector.valid) found++;
    }

    printf("\n  Track 0 side 0: %d/%d sectors via floppy_read_sector\n  ",
           found, SECTORS_PER_TRACK);
    ASSERT(found >= 16);
}

TEST(test_pio_f12_mount_and_list) {
    setup_floppy();

    f12_io_t io = {
        .read = floppy_io_read,
        .write = floppy_io_write,
        .disk_changed = floppy_io_disk_changed,
        .write_protected = floppy_io_write_protected,
        .ctx = &floppy,
    };

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    f12_err_t err = f12_mount(&fs, io);
    ASSERT_EQ(err, F12_OK);

    printf("\n  F12 mounted via PIO simulator\n");

    f12_dir_t dir;
    f12_opendir(&fs, "/", &dir);

    f12_stat_t stat;
    int count = 0;
    while (f12_readdir(&dir, &stat) == F12_OK) {
        printf("  %-12s %8u bytes\n", stat.name, stat.size);
        count++;
    }
    f12_closedir(&dir);

    printf("  Total: %d files\n  ", count);
    ASSERT(count > 0);

    f12_unmount(&fs);
}

TEST(test_pio_f12_read_file) {
    setup_floppy();

    f12_io_t io = {
        .read = floppy_io_read,
        .write = floppy_io_write,
        .disk_changed = floppy_io_disk_changed,
        .write_protected = floppy_io_write_protected,
        .ctx = &floppy,
    };

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    f12_mount(&fs, io);

    f12_file_t *f = f12_open(&fs, "README.SS", "r");
    ASSERT(f != NULL);

    f12_stat_t stat;
    f12_stat(&fs, "README.SS", &stat);

    uint8_t *buf = malloc(stat.size + 1);
    uint32_t total = 0;
    int n;
    while ((n = f12_read(f, buf + total, 512)) > 0) total += n;
    f12_close(f);

    buf[total] = '\0';
    printf("\n  README.SS: %u bytes via PIO simulator\n", total);
    printf("  First 60 chars: %.60s\n  ", buf);

    ASSERT_EQ(total, stat.size);
    ASSERT(total > 0);

    free(buf);
    f12_unmount(&fs);
}

static void replace_track_with_encoding(uint8_t cyl, uint8_t side,
                                        uint8_t claim_track, uint8_t claim_side) {
    track_t t;
    memset(&t, 0, sizeof(t));
    t.track = claim_track;
    t.side = claim_side;
    for (int i = 0; i < SECTORS_PER_TRACK; i++) {
        t.sectors[i].track = claim_track;
        t.sectors[i].side = claim_side;
        t.sectors[i].sector_n = i + 1;
        t.sectors[i].size_code = 2;
        t.sectors[i].valid = true;
        memset(t.sectors[i].data, 0xCD, SECTOR_SIZE);
    }

    static uint8_t pulse_buf[200000];
    mfm_encode_t enc;
    mfm_encode_init(&enc, pulse_buf, sizeof(pulse_buf));
    mfm_encode_track(&enc, &t);

    pio_sim_track_t *dst = &sim_drive.tracks[cyl][side];
    free(dst->deltas);
    dst->deltas = malloc(enc.pos * sizeof(uint16_t));
    dst->count = enc.pos;
    for (size_t i = 0; i < enc.pos; i++) {
        dst->deltas[i] = pulse_buf[i] + MFM_PIO_OVERHEAD;
    }
}

TEST(test_pio_wrong_track_detection) {
    setup_floppy();

    replace_track_with_encoding(5, 0, 7, 0);

    sector_t sector;
    sector.track = 5;
    sector.side = 0;
    sector.sector_n = 1;
    sector.valid = false;

    floppy_status_t status = floppy_read_sector(&floppy, &sector);
    ASSERT_EQ(status, FLOPPY_ERR_WRONG_TRACK);
    ASSERT(floppy.stats.wrong_track > 0);
}

TEST(test_pio_wrong_side_detection) {
    setup_floppy();

    replace_track_with_encoding(6, 0, 6, 1);

    sector_t sector;
    sector.track = 6;
    sector.side = 0;
    sector.sector_n = 1;
    sector.valid = false;

    floppy_status_t status = floppy_read_sector(&floppy, &sector);
    ASSERT_EQ(status, FLOPPY_ERR_WRONG_SIDE);
    ASSERT(floppy.stats.wrong_side > 0);
}

TEST(test_pio_seek_clamps_above_max) {
    setup_floppy();

    floppy_status_t s = floppy_seek(&floppy, 200);
    ASSERT_EQ(s, FLOPPY_OK);
    ASSERT_EQ(floppy_current_track(&floppy), FLOPPY_TRACKS - 1);
}

TEST(test_pio_motor_off_is_idempotent) {
    setup_floppy();
    floppy_motor_off(&floppy);
    floppy_motor_off(&floppy);
    ASSERT(!floppy.motor_on);
}

TEST(test_pio_select_idempotent_no_op) {
    setup_floppy();
    floppy_select(&floppy, true);
    floppy_select(&floppy, true);
    ASSERT(floppy.selected);
    floppy_select(&floppy, false);
    floppy_select(&floppy, false);
    ASSERT(!floppy.selected);
}

TEST(test_pio_at_track0_and_state) {
    setup_floppy();
    floppy_seek(&floppy, 0);
    ASSERT(floppy_at_track0(&floppy));
    floppy_seek(&floppy, 5);
    ASSERT(!floppy_at_track0(&floppy));

    bool dc = floppy_disk_changed(&floppy);
    (void)dc;

    floppy_stats_reset(&floppy);
    floppy_stats_t stats = floppy_stats(&floppy);
    ASSERT_EQ(stats.reads, 0);
}

TEST(test_pio_set_density) {
    setup_floppy();
    floppy_set_density(&floppy, true);
    floppy_set_density(&floppy, false);
}

int main(void) {
    size_t scp_size;
    uint8_t *scp_data = load_file(SCP_PATH, &scp_size);
    if (!scp_data) {
        printf("=== PIO Simulator Tests: SKIPPED (no SCP file) ===\n");
        return 0;
    }

    pio_sim_init(&sim_drive);
    pio_sim_load_scp(&sim_drive, scp_data, scp_size);
    pio_sim_install(&sim_drive);

    printf("=== PIO Simulator Tests ===\n");
    printf("Full firmware path: floppy.c -> mfm_decode.c -> fat12.c -> f12.c\n\n");

    RUN_TEST(test_pio_read_sector);
    RUN_TEST(test_pio_read_all_track0);
    RUN_TEST(test_pio_f12_mount_and_list);
    RUN_TEST(test_pio_f12_read_file);
    RUN_TEST(test_pio_seek_clamps_above_max);
    RUN_TEST(test_pio_motor_off_is_idempotent);
    RUN_TEST(test_pio_select_idempotent_no_op);
    RUN_TEST(test_pio_at_track0_and_state);
    RUN_TEST(test_pio_set_density);
    RUN_TEST(test_pio_wrong_track_detection);
    RUN_TEST(test_pio_wrong_side_detection);

    pio_sim_free(&sim_drive);
    free(scp_data);

    TEST_RESULTS();
}
