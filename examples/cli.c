#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "floppy.h"
#include "f12.h"

// ============== Forward Declarations ==============

static f12_err_t do_mount(f12_t *fs, floppy_t *floppy);

// ============== CLI Helpers ==============

static void print_help(void) {
  printf("\n=== Floppy CLI ===\n");
  printf("  l - List files\n");
  printf("  r - Read TEST.TXT\n");
  printf("  w - Write TEST.TXT\n");
  printf("  d - Delete TEST.TXT\n");
  printf("  f - Format disk (quick)\n");
  printf("  F - Format disk (full)\n");
  printf("  s - Show disk status\n");
  printf("  m - Mount/remount\n");
  printf("  u - Unmount\n");
  printf("  h - Help\n");
  printf("> ");
}

static void cmd_status(floppy_t *floppy) {
  printf("\n=== Disk Status ===\n");
  printf("  Write protected: %s\n", floppy_write_protected(floppy) ? "YES" : "no");
  printf("  Disk changed:    %s\n", floppy_disk_changed(floppy) ? "YES" : "no");
  printf("  Current track:   %d\n", floppy_current_track(floppy));
  printf("  At track 0:      %s\n", floppy_at_track0(floppy) ? "yes" : "no");
  printf("  Motor:           %s\n", floppy->motor_on ? "ON" : "off");
  printf("  Selected:        %s\n", floppy->selected ? "yes" : "no");
  if (floppy->motor_on) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t idle_ms = now - floppy->last_io_time_ms;
    uint32_t remaining = (idle_ms < FLOPPY_IDLE_TIMEOUT_MS) ?
                         (FLOPPY_IDLE_TIMEOUT_MS - idle_ms) / 1000 : 0;
    printf("  Idle:            %lus (off in %lus)\n", idle_ms / 1000, remaining);
  }
}

static void cmd_list(f12_t *fs) {
  f12_dir_t dir;
  f12_stat_t stat;

  printf("\n=== Directory ===\n");
  f12_err_t err = f12_opendir(fs, "/", &dir);
  if (err != F12_OK) {
    printf("Error: %s\n", f12_strerror(err));
    return;
  }

  int count = 0;
  while (f12_readdir(&dir, &stat) == F12_OK) {
    printf("  %-12s %8lu %s\n", stat.name, stat.size, stat.is_dir ? "<DIR>" : "");
    count++;
  }
  f12_closedir(&dir);

  if (count == 0) {
    printf("  (empty)\n");
  }
}

static void cmd_read(f12_t *fs) {
  printf("\nReading TEST.TXT...\n");

  f12_file_t *file = f12_open(fs, "TEST.TXT", "r");
  if (!file) {
    printf("Error: %s\n", f12_strerror(f12_errno(fs)));
    return;
  }

  char buf[256];
  int total = 0;
  int n;

  printf("--- Contents ---\n");
  while ((n = f12_read(file, buf, sizeof(buf) - 1)) > 0) {
    buf[n] = '\0';
    printf("%s", buf);
    total += n;
  }
  printf("\n--- End (%d bytes) ---\n", total);

  f12_close(file);
}

static void cmd_write(f12_t *fs) {
  printf("\nWriting TEST.TXT...\n");

  f12_file_t *file = f12_open(fs, "TEST.TXT", "w");
  if (!file) {
    printf("Error: %s\n", f12_strerror(f12_errno(fs)));
    return;
  }

  const char *msg = "Hello from Pico floppy!\nThis is a test file.\nLine 3.\n";
  int n = f12_write(file, msg, strlen(msg));

  if (n < 0) {
    printf("Write error: %s\n", f12_strerror(f12_errno(fs)));
  } else {
    printf("Wrote %d bytes\n", n);
  }

  f12_close(file);
}

static void cmd_delete(f12_t *fs) {
  printf("\nDeleting TEST.TXT...\n");

  f12_err_t err = f12_delete(fs, "TEST.TXT");
  if (err != F12_OK) {
    printf("Error: %s\n", f12_strerror(err));
  } else {
    printf("Deleted.\n");
  }
}

static void cmd_format(f12_t *fs, floppy_t *floppy, bool full, bool *mounted) {
  printf("\nFormatting disk (%s)...\n", full ? "full" : "quick");
  printf("This will erase all data! Press 'y' to confirm: ");

  int c;
  while ((c = getchar()) == EOF) {
    tight_loop_contents();
  }
  printf("%c\n", c);

  if (c != 'y' && c != 'Y') {
    printf("Cancelled.\n");
    return;
  }

  // Unmount if mounted
  if (*mounted) {
    f12_unmount(fs);
    *mounted = false;
  }

  // Set up io for format (works even when unmounted)
  fs->io = (f12_io_t){
    .read = floppy_io_read,
    .write = floppy_io_write,
    .disk_changed = floppy_io_disk_changed,
    .write_protected = floppy_io_write_protected,
    .ctx = floppy,
  };

  f12_err_t err = f12_format(fs, "PICODISK", full);
  if (err != F12_OK) {
    printf("Format error: %s\n", f12_strerror(err));
  } else {
    printf("Format complete.\n");
    // Auto-mount after format
    printf("Mounting...\n");
    f12_err_t merr = do_mount(fs, floppy);
    if (merr == F12_OK) {
      printf("Mounted.\n");
      *mounted = true;
    } else {
      printf("Mount error: %s\n", f12_strerror(merr));
    }
  }
}

static f12_err_t do_mount(f12_t *fs, floppy_t *floppy) {
  f12_io_t io = {
    .read = floppy_io_read,
    .write = floppy_io_write,
    .disk_changed = floppy_io_disk_changed,
    .write_protected = floppy_io_write_protected,
    .ctx = floppy,
  };

  return f12_mount(fs, io);
}

static void cmd_mount(f12_t *fs, floppy_t *floppy, bool *mounted) {
  printf("\nMounting...\n");

  if (*mounted) {
    f12_unmount(fs);
    *mounted = false;
  }

  f12_err_t err = do_mount(fs, floppy);
  if (err != F12_OK) {
    printf("Mount error: %s\n", f12_strerror(err));
  } else {
    printf("Mounted.\n");
    *mounted = true;
  }
}

static void cmd_unmount(f12_t *fs, bool *mounted) {
  printf("\nUnmounting...\n");

  if (*mounted) {
    f12_unmount(fs);
    *mounted = false;
    printf("Unmounted.\n");
  } else {
    printf("Not mounted.\n");
  }
}

// ============== Main ==============

int main(void) {
  stdio_init_all();
  sleep_ms(2000);

  printf("\n\n=== Pico Floppy Controller ===\n");

  // Initialize floppy drive
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
  floppy_set_density(&floppy, true);  // HD mode

  printf("Floppy initialized.\n");

  // Mount filesystem
  f12_t fs;
  bool mounted = false;

  f12_err_t err = do_mount(&fs, &floppy);
  if (err == F12_OK) {
    printf("Filesystem mounted.\n");
    mounted = true;
  } else {
    printf("Mount failed: %s\n", f12_strerror(err));
    printf("Insert a formatted disk and press 'm' to mount.\n");
  }

  print_help();

  // Main loop
  for (;;) {
    int c = getchar();
    if (c == EOF) {
      tight_loop_contents();
      continue;
    }

    // Skip whitespace
    if (c == '\n' || c == '\r' || c == ' ') {
      continue;
    }

    printf("%c\n", c);  // Echo command

    // Commands that don't need mount
    switch (c) {
      case 'h':
      case '?':
        print_help();
        continue;

      case 's':
        cmd_status(&floppy);
        printf("> ");
        continue;

      case 'm':
        cmd_mount(&fs, &floppy, &mounted);
        printf("> ");
        continue;

      case 'u':
        cmd_unmount(&fs, &mounted);
        printf("> ");
        continue;
    }

    // Format doesn't need mount
    if (c == 'f' || c == 'F') {
      cmd_format(&fs, &floppy, c == 'F', &mounted);
      printf("> ");
      continue;
    }

    // Commands that need mount
    if (!mounted) {
      printf("Error: Not mounted. Press 'm' to mount.\n> ");
      continue;
    }

    switch (c) {
      case 'l':
        cmd_list(&fs);
        break;

      case 'r':
        cmd_read(&fs);
        break;

      case 'w':
        cmd_write(&fs);
        break;

      case 'd':
        cmd_delete(&fs);
        break;

      default:
        printf("Unknown command '%c'. Press 'h' for help.\n", c);
        break;
    }

    printf("> ");
  }

  return 0;
}
