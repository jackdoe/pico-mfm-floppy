#include "test.h"
#include "pio_sim.h"
#include "flux_sim.h"
#include "vdisk.h"
#include "../src/floppy.h"
#include "../src/fat12.h"
#include "../src/f12.h"

floppy_t *pio_sim_floppy_ref;

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

static void setup_formatted_disk(void) {
    static uint8_t disk_sectors[2880][512];

    vdisk_t vdisk;
    vdisk_init(&vdisk);
    fat12_io_t fat_io = { .read = vdisk_read, .write = vdisk_write, .ctx = &vdisk };
    fat12_format(fat_io, "VERIFY", true);
    memcpy(disk_sectors, vdisk.data, sizeof(disk_sectors));

    size_t scp_size;
    uint8_t *scp_data = scp_encode_disk(disk_sectors, &scp_size);

    pio_sim_free(&sim_drive);
    pio_sim_init(&sim_drive);
    pio_sim_load_scp(&sim_drive, scp_data, scp_size);
    pio_sim_install(&sim_drive);

    free(scp_data);

    setup_floppy();
}

static f12_io_t make_floppy_io(void) {
    return (f12_io_t){
        .read = floppy_io_read,
        .read_track = floppy_io_read_track,
        .write = floppy_io_write,
        .disk_changed = floppy_io_disk_changed,
        .write_protected = floppy_io_write_protected,
        .ctx = &floppy,
    };
}

TEST(test_write_verify_success) {
    setup_formatted_disk();

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    ASSERT_EQ(f12_mount(&fs, make_floppy_io()), F12_OK);

    f12_file_t *f = f12_open(&fs, "HELLO.TXT", "w");
    ASSERT(f != NULL);
    const char *msg = "Write verification test - every byte must survive encode, flux write, readback, decode.";
    int n = f12_write(f, msg, strlen(msg));
    ASSERT_EQ(n, (int)strlen(msg));
    ASSERT_EQ(f12_close(f), F12_OK);

    f12_unmount(&fs);
    ASSERT_EQ(f12_mount(&fs, make_floppy_io()), F12_OK);

    f = f12_open(&fs, "HELLO.TXT", "r");
    ASSERT(f != NULL);
    char buf[256];
    n = f12_read(f, buf, sizeof(buf));
    ASSERT_EQ(n, (int)strlen(msg));
    ASSERT_MEM_EQ(buf, msg, strlen(msg));
    f12_close(f);

    f12_unmount(&fs);
}

TEST(test_write_verify_large_file) {
    setup_formatted_disk();

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    ASSERT_EQ(f12_mount(&fs, make_floppy_io()), F12_OK);

    f12_file_t *f = f12_open(&fs, "BIG.DAT", "w");
    ASSERT(f != NULL);

    uint8_t pattern[5000];
    for (int i = 0; i < 5000; i++) pattern[i] = (i * 7 + 13) & 0xFF;

    uint32_t written = 0;
    while (written < sizeof(pattern)) {
        uint32_t chunk = sizeof(pattern) - written;
        if (chunk > 512) chunk = 512;
        int n = f12_write(f, pattern + written, chunk);
        ASSERT(n > 0);
        written += n;
    }
    ASSERT_EQ(f12_close(f), F12_OK);

    f12_unmount(&fs);
    ASSERT_EQ(f12_mount(&fs, make_floppy_io()), F12_OK);

    f12_stat_t stat;
    ASSERT_EQ(f12_stat(&fs, "BIG.DAT", &stat), F12_OK);
    ASSERT_EQ(stat.size, 5000);

    f = f12_open(&fs, "BIG.DAT", "r");
    ASSERT(f != NULL);

    uint8_t buf[5000];
    uint32_t total = 0;
    int n;
    while ((n = f12_read(f, buf + total, 512)) > 0) total += n;
    f12_close(f);

    ASSERT_EQ(total, 5000);
    ASSERT_MEM_EQ(buf, pattern, 5000);

    f12_unmount(&fs);
}

TEST(test_write_verify_retry) {
    setup_formatted_disk();
    sim_drive.fault_writes_remaining = 1;

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    ASSERT_EQ(f12_mount(&fs, make_floppy_io()), F12_OK);

    f12_file_t *f = f12_open(&fs, "RETRY.TXT", "w");
    ASSERT(f != NULL);
    const char *msg = "This data must survive a transient write failure.";
    int n = f12_write(f, msg, strlen(msg));
    ASSERT_EQ(n, (int)strlen(msg));
    ASSERT_EQ(f12_close(f), F12_OK);

    ASSERT_EQ(sim_drive.fault_writes_remaining, 0);

    f12_unmount(&fs);
    ASSERT_EQ(f12_mount(&fs, make_floppy_io()), F12_OK);

    f = f12_open(&fs, "RETRY.TXT", "r");
    ASSERT(f != NULL);
    char buf[256];
    n = f12_read(f, buf, sizeof(buf));
    ASSERT_EQ(n, (int)strlen(msg));
    ASSERT_MEM_EQ(buf, msg, strlen(msg));
    f12_close(f);

    f12_unmount(&fs);
}

TEST(test_write_verify_permanent_fail) {
    setup_formatted_disk();
    sim_drive.fault_writes_remaining = 100;

    f12_t fs;
    memset(&fs, 0, sizeof(fs));
    ASSERT_EQ(f12_mount(&fs, make_floppy_io()), F12_OK);

    f12_file_t *f = f12_open(&fs, "FAIL.TXT", "w");
    ASSERT(f != NULL);
    int n = f12_write(f, "doomed", 6);
    ASSERT_EQ(n, 6);

    f12_err_t err = f12_close(f);
    ASSERT_EQ(err, F12_ERR_IO);

    f12_unmount(&fs);
}

int main(void) {
    printf("=== Write Verification Tests ===\n\n");

    RUN_TEST(test_write_verify_success);
    RUN_TEST(test_write_verify_large_file);
    RUN_TEST(test_write_verify_retry);
    RUN_TEST(test_write_verify_permanent_fail);

    pio_sim_free(&sim_drive);

    TEST_RESULTS();
}
