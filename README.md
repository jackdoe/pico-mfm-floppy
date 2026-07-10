# Pico MFM Floppy

A bare-metal 1.44 MB 3.5-inch floppy driver and FAT12 filesystem for Raspberry Pi Pico and Pico 2. It uses PIO, DMA, and software MFM encoding instead of a floppy controller.

![Raspberry Pi Pico 2 connected to a 3.5-inch floppy drive](picture.jpg)

## Start here

The Pico is a 3.3 V device and is not 5 V tolerant. Read [Hardware](#hardware) before connecting a drive. Power the drive from a separate regulated 5 V supply, join the Pico and drive grounds, and pull `/READ_DATA` up to 3.3 V rather than 5 V.

1. Wire the drive using the pin table below.
2. Build the UF2 for the exact board using [Build firmware](#build-firmware).
3. Hold BOOTSEL while connecting the Pico, then copy `floppy_cli.uf2` to the mounted `RPI-RP2` volume.
4. Open the Pico USB CDC serial port with a terminal that sends CR or LF. UART output is disabled and the USB port does not depend on a baud rate.
5. Insert a disk and run `status`, then `mount`, then `ls`.

Mounting is read-only. Do not run `format`, `test-full`, or `crashtest` on media that must be preserved. Start with `diskdump quiet` when evaluating an unknown disk without modifying it.

## Architecture

```text
Application and USB serial CLI
    |
    v
f12 handle API
    |
    v
Track cache and media-generation boundary
    |
    v
FAT12 metadata, copy-on-write files, and fsck
    |
    v
Typed block-device interface
    |
    v
Drive control, recovery, write verification
    |
    v
MFM codec, DMA rings, generated PIO programs
    |
    v
34-pin floppy drive
```

The disk geometry is fixed and explicit: 80 cylinders, two heads, 18 sectors per track, and 512 bytes per sector. Sector identifiers used by the internal API are zero-based.

The firmware is split into:

- `block.h`: geometry, partial-track representation, and typed block status.
- `flux_read.pio` and `flux_write.pio`: timed flux capture and generation programs compiled by `pioasm`.
- `crc.c`: the table used for MFM sector CRC-16.
- `floppy.c`: resource ownership, motor and head control, media detection, flux DMA, recovery, and verified track writes.
- `mfm_decode.c` and `mfm_encode.c`: adaptive MFM decoding and streaming encoding with inner-track precompensation.
- `fat12.c`: FAT12 names, directories, cluster chains, metadata batching, formatting, transactional replacement, and repair.
- `f12.c`: mounted-filesystem state, opaque checked handles, track caching, file operations, and error translation.
- `examples/cli.c`: USB serial administration, diagnostics, destructive tests, and power-cut verification.

The CMake target `floppy_lib` is a static library. It owns its C sources and both generated PIO headers; consumers receive only the public include and link requirements.

## Reliability model

Every layer has one error vocabulary for its abstraction. Block operations return `block_status_t`; file reads and writes return both a typed error and the number of bytes transferred. Partial progress is never disguised as complete success.

The drive serializes operations and gives control, read, write, raw-flux, and teardown paths bounded deadlines. A media generation is captured before I/O and checked throughout the operation, so removal or replacement invalidates cached data and open filesystem state. Reads reject conflicting copies of a sector and recovery is limited to explicit recalibration and head-jog attempts.

Writes stream MFM transitions through DMA, then read the track back and compare it with the requested image. Write protection, media changes, FIFO underruns, overruns, timeouts, CRC failures, wrong-track data, and verification failures remain distinct.

Every successful `f12_init` establishes a fresh context incarnation and invalidates every handle issued by an earlier initialization. File handles also carry mount generation, slot generation, and slot identity, so stale, forged, reused, and post-remount handles are rejected. A failed writer close retains enough state to retry the same commit; callers must either retry `f12_close` or explicitly abort.

`floppy_t` and `f12_t` are large, aligned owner contexts intended for static storage. `floppy_init` accepts fresh storage, permits one active hardware context, rejects live reinitialization, and requires `floppy_deinit` before that storage is released or reused. `f12_init` is an explicit reset. `f12_is_mounted` is a typed query so an observer failure cannot be mistaken for an unmounted filesystem. Hardware operations are serialized, but callers must not race context initialization or teardown, and filesystem calls require application-level serialization across cores.

Replacing a file publishes the new directory entry before reclaiming the old chain. Mount is read-only. Repair is an explicit `fsck` operation, and a repair is considered successful only when a subsequent read-only scan converges to a clean report. FAT-copy ambiguity is reported rather than guessed.

## Hardware

The default CLI pin assignment is:

| Floppy pin | Signal | Pico GPIO | Direction |
|---:|---|---:|---|
| 2 | /DENSITY | GP15 | Pico to drive |
| 8 | /INDEX | GP14 | Drive to Pico |
| 12 | /DRIVE_SELECT_B | GP12 | Pico to drive |
| 16 | /MOTOR_ENABLE_B | GP10 | Pico to drive |
| 18 | /DIRECTION | GP9 | Pico to drive |
| 20 | /STEP | GP8 | Pico to drive |
| 22 | /WRITE_DATA | GP7 | Pico to drive |
| 24 | /WRITE_GATE | GP6 | Pico to drive |
| 26 | /TRACK_0 | GP5 | Drive to Pico |
| 28 | /WRITE_PROTECT | GP4 | Drive to Pico |
| 30 | /READ_DATA | GP3 | Drive to Pico |
| 32 | /SIDE_SELECT | GP2 | Pico to drive |
| 34 | /DISK_CHANGE | GP1 | Drive to Pico |

Connect every odd-numbered floppy pin to ground. Power the drive from a separate regulated 5 V supply sized for its spin-up current, and connect the drive and Pico grounds.

The firmware drives control signals as open-drain outputs. Do not expose a Pico GPIO to 5 V. `/READ_DATA` needs an external pull-up to 3.3 V; 4.7 kΩ is the tested value. Check the electrical requirements of the exact drive before connecting it.

## Timing

The CLI selects clocks that make the PIO dividers integral:

| Board | System clock | Read PIO | Read divider | Write PIO | Write divider |
|---|---:|---:|---:|---:|---:|
| Pico / RP2040 | 144 MHz | 72 MHz | 2 | 24 MHz | 6 |
| Pico 2 / RP2350 | 144 MHz | 72 MHz | 2 | 24 MHz | 6 |

`floppy_init` rejects clocks that are not an exact nonzero multiple of 72 MHz, so the read and write PIO dividers stay integral. 72 MHz is the only in-spec RP2040 multiple, and it is too slow to drain the flux DMA ring in real time, so the CLI runs the RP2040 at 144 MHz (above its 133 MHz datasheet rating) with the core voltage raised to 1.20 V. 133 MHz is not a multiple of 72 MHz and would need a jittery fractional divider.

The CLI also links with `copy_to_ram`, so the whole program runs from SRAM rather than executing in place from QSPI flash, removing XIP cache-miss stalls from the real-time flux read loop.

## Hardware validation

Use a disposable, writable 1.44 MB disk for the destructive hardware suite:

```text
test-full 12
```

This formats every track, exercises raw-flux ownership, rejects concurrent motion and teardown, verifies stable media generation, performs filesystem operations with varied transfer sizes, fills every free cluster with deterministic randomized files, verifies exact disk-full transactions, deletes files across the disk, refills the fragmented space to capacity, observes verified DMA track writes, remounts, requires a clean filesystem check, and reads all 2,880 sectors. A successful run ends with `ALL PASSED` and leaves the drive unmounted, deselected, and stopped.

Test the physical disk-change path with a mounted canonical FAT12 disk and a second canonical FAT12 disk ready:

```text
test-media
```

At the first prompt, eject the mounted disk, leave the drive empty, and enter `y`. After the stale-state checks pass, insert the replacement disk and confirm the second prompt. The command requires one exact generation advance, rejection of the stale hardware generation, invalidation of an open filesystem handle, successful latch clearing through a physical step, a clean mount of the replacement disk, and safe cleanup.

Use `crashtest`, cut power during its overwrite loop, reboot, and run `crashcheck` for physical power-loss validation. The check accepts only the recorded old or new target generation and requires the stable and filler files to remain exact.

## Build firmware

Install the Pico SDK and export `PICO_SDK_PATH`.

```sh
cmake -S . -B build-rp2040 -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release
cmake --build build-rp2040 --parallel
```

The RP2040 UF2 is `build-rp2040/floppy_cli.uf2`.

```sh
cmake -S . -B build-rp2350 -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release
cmake --build build-rp2350 --parallel
```

The RP2350 UF2 is `build-rp2350/floppy_cli.uf2`.

Both builds require C11 for project C and C++17 for Pico SDK dependencies. Project sources compile with warnings treated as errors, including pedantic, conversion, sign-conversion, shadow, format, undefined-macro, and strict-prototype diagnostics.

## Run host tests

The host suite requires:

- a C11 compiler and CMake;
- `pioasm` on `PATH` or in the Pico SDK build locations searched by CMake;
- the nonempty SCP fixture at `system-shock-multilingual-floppy-ibm-pc/disk1.scp`, or an explicit replacement through `SCP_FIXTURE`.

```sh
./tests/run_all.sh
```

The runner fails if configuration registers zero tests. It exercises the MFM codec, FAT12 and f12 APIs, malformed media, fuzz cases, SCP decoding and round trips, PIO simulation, PIO instruction emulation, timeout and underrun paths, write verification, and end-to-end corruption recovery.

Run the same suite with sanitizers:

```sh
./tests/run_all.sh -DENABLE_SANITIZERS=ON
```

Increase deterministic SCP round-trip coverage:

```sh
./tests/run_all.sh -DSCP_ITERATIONS=100
```

Use another capture:

```sh
SCP_FIXTURE=/absolute/path/to/disk.scp ./tests/run_all.sh
```

Generate coverage reports with `gcovr`:

```sh
./tests/run_coverage.sh
```

## Simulation model

The host suite tests three different boundaries:

- `flux_sim` decodes synthetic tracks and the bundled SCP capture as transition intervals.
- `pio_emu` executes the generated PIO instructions and checks their cycle-level behavior.
- `pio_sim` models GPIO, index pulses, head motion, DMA rings, FIFOs, write gating, media changes, write protection, torn writes, stalls, and read-back verification while running the production floppy driver.

Flux and PIO simulation use the same seeded timing-noise model. It can combine independent transition jitter, fixed spindle-rate error, bounded correlated speed wander, and sparse impulse displacement. The suite proves that identical seeds reproduce identical waveforms, modest noise still decodes exact sector data from synthetic and real SCP tracks, noise reaches the production PIO/DMA read path, and excessive noise is rejected instead of producing accepted corrupt data. Noise is disabled unless a test configures it, so unrelated tests remain deterministic.

The simulator does not claim to model analog voltage thresholds, cable reflections, grounding faults, motor torque, head alignment, magnetic media physics, or real multicore and interrupt timing. Use `test-full`, `test-media`, and the power-cut procedure for those hardware boundaries.

## CLI

The firmware exposes a USB serial CLI. `help` prints the exact command grammar. Filenames are canonical FAT12 8.3 names; invalid or overlong input is rejected instead of truncated.

`write` and `cp` retain a writer whose final commit failed. Run `commit` to resume that commit. Unmount, format, destructive tests, and reboot refuse to silently discard it.

`fsck` reports every detected defect. `fsck fix` repairs and immediately checks convergence. `crashtest` repeatedly replaces a target file for physical power-cut testing; after reboot, `crashcheck` requires the stable and filler files to be exact and the target to be exactly either the recorded old value or the recorded new value.

`format`, `test-full`, and `crashtest` destroy data. Use media that can be erased.

## License

MIT
