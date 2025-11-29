# Pico Floppy Driver

> **Note**: Most of the code and all of this README were written by **Claude Opus 4.5**.
> Verified and tested on real hardware by **jackdoe**.

A bare-metal 3.5" floppy disk driver for the Raspberry Pi Pico, implementing MFM encoding/decoding and FAT12 filesystem support without any dedicated floppy disk controller hardware.

```
 ______
| |__| |
|  ()  |
|______|
```

## Table of Contents

1. [Overview](#overview)
2. [Hardware](#hardware)
   - [Floppy Drive Basics](#floppy-drive-basics)
   - [Wiring](#wiring)
   - [Pull-up Resistors](#pull-up-resistors)
   - [Power](#power)
3. [MFM Encoding](#mfm-encoding)
   - [Why Encoding?](#why-encoding)
   - [FM Encoding](#fm-encoding)
   - [MFM Encoding](#mfm-encoding-1)
   - [Pulse Intervals](#pulse-intervals)
   - [The Sync Problem](#the-sync-problem)
   - [The Sync Pattern](#the-sync-pattern)
4. [Disk Format](#disk-format)
   - [Physical Layout](#physical-layout)
   - [Track Format](#track-format)
   - [Sector Format](#sector-format)
5. [Implementation](#implementation)
   - [Architecture](#architecture)
   - [PIO Programs](#pio-programs)
   - [MFM Decoder](#mfm-decoder)
   - [MFM Encoder](#mfm-encoder)
   - [Floppy Driver](#floppy-driver)
   - [FAT12 Filesystem](#fat12-filesystem)
   - [F12 API](#f12-api)
6. [Usage](#usage)
7. [Design Decisions](#design-decisions)
8. [References](#references)

---

## Overview

This project bit-bangs a 3.5" floppy drive using the RP2040's PIO (Programmable I/O) state machines. Traditional floppy disk controllers (like the NEC μPD765 or Intel 82077) handled all the timing-critical MFM encoding/decoding in hardware. This driver does it all in software, using PIO only for precise pulse measurement and generation.

```
┌─────────────────────────────────────────────────────────────────┐
│                         Application                             │
├─────────────────────────────────────────────────────────────────┤
│                          f12 API                                │
│              f12_open() f12_read() f12_write()                  │
│                    ↓              ↓                             │
│              ┌─────────────────────────┐                        │
│              │       LRU Cache         │  36 sectors (2 tracks) │
│              └─────────────────────────┘                        │
├─────────────────────────────────────────────────────────────────┤
│                         fat12                                   │
│         Boot sector, FAT tables, directories, clusters          │
├─────────────────────────────────────────────────────────────────┤
│                         floppy                                  │
│     Motor control, head seek, side select, read/write sectors   │
├─────────────────────────────────────────────────────────────────┤
│                    mfm_decode / mfm_encode                      │
│              Flux pulse intervals ←→ data bytes                 │
├─────────────────────────────────────────────────────────────────┤
│                    PIO State Machines                           │
│         flux_read: measure pulse intervals (24 MHz)             │
│         flux_write: generate pulse intervals (24 MHz)           │
├─────────────────────────────────────────────────────────────────┤
│                      Physical Drive                             │
│            Magnetic flux transitions on spinning disk           │
└─────────────────────────────────────────────────────────────────┘
```

---

## Hardware

### Floppy Drive Basics

A 3.5" HD (High Density) floppy disk stores **1.44 MB** of data:

```
                    Floppy Disk Cross-Section

         Track 0 (outer edge)      
              │                    
              ▼                    
    ┌─────────────────────────────────────────────────┐
    │  ┌───────────────────────────────────────────┐  │
    │  │  ┌─────────────────────────────────────┐  │  │
    │  │  │  ┌───────────────────────────────┐  │  │  │
    │  │  │  │           ...                 │  │  │  │
    │  │  │  │     ┌─────────────────┐       │  │  │  │
    │  │  │  │     │                 │       │  │  │  │
    │  │  │  │     │     (center)    │       │  │  │  │
    │  │  │  │     │                 │       │  │  │  │
    │  │  │  │     └─────────────────┘       │  │  │  │
    │  │  │  │                               │  │  │  │
    │  │  │  └───────────────────────────────┘  │  │  │
    │  │  └─────────────────────────────────────┘  │  │
    │  └───────────────────────────────────────────┘  │
    └─────────────────────────────────────────────────┘

    80 tracks × 2 sides × 18 sectors × 512 bytes = 1,474,560 bytes
```

The disk spins at **300 RPM** (one rotation = 200ms). A read/write head floats on an air cushion ~0.1μm above the magnetic surface. The stepper motor moves the head between tracks.

### Wiring

Floppy drives use a 34-pin interface. The odd pins (bottom row) are all **ground**. The even pins (top row) are signals:

```
    Floppy Drive 34-Pin Connector (Active-Low, accent on certain pins)
    ═══════════════════════════════════════════════════════════════════

    Pin  Signal              Dir    Description
    ───  ──────────────────  ───    ───────────────────────────────────
     2   /DENSITY            OUT    Density select (active low = HD)
     8   /INDEX              IN     Pulses low once per rotation
    10   /MOTOR_ENABLE       OUT    Spin motor (active low)
    12   /DRIVE_SELECT_1     OUT    Select this drive (active low)
    14   /DRIVE_SELECT_0     OUT    (usually active, active low)
    16   /MOTOR_ENABLE_0     OUT    (unused in single drive)
    18   /DIRECTION          OUT    Step direction (low = inward)
    20   /STEP               OUT    Pulse low to step one track
    22   /WRITE_DATA         OUT    Pulse low for flux transition
    24   /WRITE_GATE         OUT    Enable writing (active low)
    26   /TRACK_0            IN     Low when head at track 0
    28   /WRITE_PROTECT      IN     Low when disk is protected
    30   /READ_DATA          IN     Pulses low on flux transitions
    32   /SIDE_SELECT        OUT    Head select (high=side 0, low=side 1)
    34   /DISK_CHANGE        IN     Low after disk removed/inserted

    All odd pins (1,3,5...33) are GROUND
```

**Important**: All signals are active-low and accent-open-drain or open-collector. The drive doesn't "push" high - it only pulls low or leaves the pin floating.

### Wiring Diagram

```
                         Raspberry Pi Pico
                        ┌─────────────────┐
                        │                 │
    Floppy Pin 8  ──────┤ GP2  (INDEX)    │
    Floppy Pin 26 ──────┤ GP3  (TRACK0)   │
    Floppy Pin 28 ──────┤ GP4  (WP)       │
    Floppy Pin 30 ──────┤ GP5  (RDATA)    │◄── 4.7kΩ to 3.3V !!
    Floppy Pin 34 ──────┤ GP6  (DSKCHG)   │
    Floppy Pin 12 ──────┤ GP7  (DRVSEL)   │
    Floppy Pin 10 ──────┤ GP8  (MOTOR)    │
    Floppy Pin 18 ──────┤ GP9  (DIR)      │
    Floppy Pin 20 ──────┤ GP10 (STEP)     │
    Floppy Pin 22 ──────┤ GP11 (WDATA)    │
    Floppy Pin 24 ──────┤ GP12 (WGATE)    │
    Floppy Pin 32 ──────┤ GP13 (SIDE)     │
    Floppy Pin 2  ──────┤ GP14 (DENSITY)  │
                        │                 │
    Floppy GND ─────────┤ GND             │
                        │                 │
                        └─────────────────┘

    DO NOT connect 5V power to GPIO!
    Power the drive from a separate 5V supply.
```

### Pull-up Resistors

The floppy interface is **open-drain/open-collector**. The drive only pulls signals LOW - it never drives HIGH. Without pull-up resistors, input lines would float and give random readings.

```
    How Open-Drain Works
    ════════════════════

    Pull-up resistor (internal or external)
           │
           ┴ R
           │
    ───────┼─────────── Signal line
           │
          ┌┴┐
          │ │ Open-drain output
          │ │ (can only sink current)
          └┬┘
           │
          GND

    When transistor OFF: Line floats → pulled HIGH by resistor
    When transistor ON:  Line pulled LOW (transistor sinks current)
```

**All input pins need pull-ups.** The driver enables the RP2040's internal pull-ups (50-80kΩ):

```c
// In floppy_init():
gpio_pull_up(pins.index);
gpio_pull_up(pins.track0);
gpio_pull_up(pins.write_protect);
gpio_pull_up(pins.read_data);
gpio_pull_up(pins.disk_change);
```

**But internal pull-ups are too weak for READ_DATA!**

The problem is the RC time constant. Parasitic capacitance from wiring (~10-20pF) combined with a weak pull-up creates slow rise times:

```
    Why READ_DATA Needs a Stronger Pull-up
    ══════════════════════════════════════

    Internal pull-up: R ≈ 50kΩ (typical RP2040)
    Parasitic capacitance: C ≈ 15pF

    Time constant τ = R × C = 50kΩ × 15pF ≈ 750ns

    But MFM pulses are only ~100-200ns wide!

         Expected signal        With weak pull-up
         ────────────────       ─────────────────
              ┌─┐                   ╭────╮
        HIGH ─┘ └─ HIGH         ────╯    ╰────
              │ │                        ↑
             100ns              Signal never fully recovers
                                before next pulse arrives!

    With 4.7kΩ external pull-up:
    τ = 4.7kΩ × 15pF ≈ 70ns  ✓  Fast enough to recover between pulses
```

**Pull-up Requirements Summary:**

```
    ┌─────────────────┬────────────┬───────────┬─────────────────────┐
    │ Pin             │ Signal     │ Frequency │ Pull-up             │
    ├─────────────────┼────────────┼───────────┼─────────────────────┤
    │ GP2  (Pin 8)    │ INDEX      │ ~5 Hz     │ Internal OK         │
    │ GP3  (Pin 26)   │ TRACK0     │ Static    │ Internal OK         │
    │ GP4  (Pin 28)   │ WR_PROTECT │ Static    │ Internal OK         │
    │ GP5  (Pin 30)   │ READ_DATA  │ ~500 kHz  │ 4.7kΩ REQUIRED !!   │
    │ GP6  (Pin 34)   │ DISK_CHANGE│ Static    │ Internal OK         │
    └─────────────────┴────────────┴───────────┴─────────────────────┘
```

For maximum reliability, add 4.7kΩ external pull-ups to ALL input pins. But READ_DATA is the only one that truly requires it.

```
    Recommended Circuit (at minimum, add R3)
    ════════════════════════════════════════

                                    3.3V
                                      │
          ┌────────────┬──────────────┼──────────────┬────────────┐
          │            │              │              │            │
         ┌┴┐          ┌┴┐            ┌┴┐            ┌┴┐          ┌┴┐
         │ │ R1       │ │ R2         │ │ R3         │ │ R4       │ │ R5
         │ │ 4.7k     │ │ 4.7k       │ │ 4.7k       │ │ 4.7k     │ │ 4.7k
         │ │(opt)     │ │(opt)       │ │(REQUIRED!) │ │(opt)     │ │(opt)
         └┬┘          └┬┘            └┬┘            └┬┘          └┬┘
          │            │              │              │            │
    INDEX─┼──GP2  TRK0─┼──GP3  RDATA──┼──GP5  WRPRT─┼──GP4  DCHG─┼──GP6
    Pin 8 │      Pin 26│        Pin 30│        Pin 28│      Pin 34│
```

**Voltage tolerance**: Since the drive only pulls low, it doesn't care if the line is pulled to 3.3V or 5V. This lets 3.3V microcontrollers interface directly with 5V-rated drives.

**Output pins**: The Pico must also use open-drain style outputs. The code does this:

```c
static void gpio_put_oc(uint pin, bool value) {
    if (value == 0) {
        gpio_put(pin, 0);
        gpio_set_dir(pin, GPIO_OUT);  // Actively drive low
    } else {
        gpio_set_dir(pin, GPIO_IN);   // High-impedance (pulled high)
    }
}
```

### Power

**CRITICAL**: The floppy drive needs 5V DC power, and can draw up to **1 amp** during seeks!

```
    Floppy Power Connector
    ══════════════════════

    ┌─────┬─────┬─────┬─────┐
    │ 5V  │ GND │ GND │ 12V │  (12V often unused on 3.5")
    └──┬──┴──┬──┴──┬──┴──┬──┘
       │     │     │     │
       │     └──┬──┘     │
       │        │        └──── Not connected (or 12V for some drives)
       │        │
       │        └──────────── Ground (connect to Pico GND too!)
       │
       └───────────────────── +5V (DO NOT connect to Pico GPIO!)

    Use a separate 5V power supply rated for 1A+
    Connect grounds together between supply, drive, and Pico
```

---

## MFM Encoding

### Why Encoding?

Raw binary data can't be written directly to magnetic media. Consider writing `0x00` (eight 0 bits):

```
    Problem: Long runs of zeros
    ═══════════════════════════

    Data:     0   0   0   0   0   0   0   0
    Flux:     ─────────────────────────────── (no transitions!)

    The read head sees NO changes for 16+ microseconds.
    Without transitions, there's nothing to synchronize to.
    Clock drift makes it impossible to know how many zeros there were.
```

We need an encoding that guarantees regular transitions for clock recovery.

### FM Encoding

### FM Encoding

FM (Frequency Modulation) was an early solution. Insert a clock bit (always 1) before every data bit:

```
    FM Encoding: Each data bit becomes [clock=1][data]
    ══════════════════════════════════════════════════

    Example: 0x3A = 0011 1010

    Data bit:        0       0       1       1       1       0       1       0
                   ┌───┐   ┌───┐   ┌───┐   ┌───┐   ┌───┐   ┌───┐   ┌───┐   ┌───┐
    FM cell:       │C D│   │C D│   │C D│   │C D│   │C D│   │C D│   │C D│   │C D│
                   │1 0│   │1 0│   │1 1│   │1 1│   │1 1│   │1 0│   │1 1│   │1 0│
                   └───┘   └───┘   └───┘   └───┘   └───┘   └───┘   └───┘   └───┘

    FM bitstream:  1 0     1 0     1 1     1 1     1 1     1 0     1 1     1 0
                   ↑       ↑       ↑ ↑     ↑ ↑     ↑ ↑     ↑       ↑ ↑     ↑
                   C       C       C D     C D     C D     C       C D     C

    Flux waveform (transition on every '1'):

         1   0   1   0   1   1   1   1   1   1   1   0   1   1   1   0
       ──┐   ┌───┐   ┌───┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌───┐   ┌─┐ ┌─┐ ┌───┐   │
         └───┘   └───┘   └─┘ └─┘ └─┘ └─┘ └─┘   └───┘ └─┘ └─┘   └───┘
         ^       ^       ^ ^ ^ ^ ^ ^ ^ ^       ^     ^ ^ ^     ^
         │       │       └─┴─┴─┴─┴─┴─┴─┘       │     └─┴─┘     │
        data=0  data=0    data=1,1,1 (fast)  data=0  data=1  data=0

    Clock transitions happen EVERY cell (the '1' at position C).
    Data transitions happen when data=1 (the '1' at position D).

    For 8 data bits: 16 FM bits with 12 transitions. Very inefficient!
```

FM doubles the bit rate (one clock bit per data bit), which halves storage capacity. MFM improves on this.

### MFM Encoding

MFM (Modified Frequency Modulation) uses smarter clock bit placement. The rule:

> **A clock bit is 1 only if BOTH the previous data bit AND current data bit are 0.**

This is a NOR gate: `clock = NOT(prev_data OR curr_data)`

```
    MFM Encoding Example: 0x3A = 0011 1010
    ═══════════════════════════════════════

    Position:         D   C   D   C   D   C   D   C   D   C   D   C   D   C   D   C
                     ─────────────────────────────────────────────────────────────────
    Data bits:        0       0       1       1       1       0       1       0
                          │       │       │       │       │       │       │       │
    Prev data:       (0)  │   0   │   0   │   1   │   1   │   1   │   0   │   1   │
    Curr data:        0   │   0   │   1   │   1   │   1   │   0   │   1   │   0   │
                          │       │       │       │       │       │       │       │
    Clock (NOR):          1       0       0       0       0       0       0       ?
                     ─────────────────────────────────────────────────────────────────
    MFM bitstream:    0   1   0   0   1   0   1   0   1   0   0   0   1   0   0   ?

    The '?' depends on the NEXT byte's first bit (MFM spans byte boundaries!)
```

Compare FM vs MFM for the same data:

```
    FM:   0 1 0 1 1 1 1 1 1 1 0 1 1 1 0 1   (16 bits, 10 transitions)
    MFM:  0 1 0 0 1 0 1 0 1 0 0 0 1 0 0 ?   (16 bits,  5 transitions)

    MFM has ~half the transitions → twice the data density!
```

### Pulse Intervals

MFM creates three distinct intervals between flux transitions:

```
    MFM Pulse Intervals
    ═══════════════════

    The MFM bitstream has transitions (flux changes) on every '1'.
    The interval between transitions determines the pattern:

    Short (2T):   "10"        ──┐ ┌──     2 bit-cells = ~2μs
                                └─┘

    Medium (3T):  "100"       ──┐   ┌──   3 bit-cells = ~3μs
                                └───┘

    Long (4T):    "1000"      ──┐     ┌── 4 bit-cells = ~4μs
                                └─────┘

    MFM guarantees: minimum 2T, maximum 4T between transitions.
    This is called a "run-length limited" code: RLL(1,3).
```

At 500 kbps (HD floppy), one bit-cell is 2μs. The decoder classifies intervals:

```c
// At 24 MHz sampling, one bit-cell ≈ 48 counts
// Thresholds set at midpoints between expected values:
m->T2_max = 57;    // ≤ 57 counts = Short  (2T, ~2μs)
m->T3_max = 82;    // ≤ 82 counts = Medium (3T, ~3μs)
                   // > 82 counts = Long   (4T, ~4μs)
```

### The Sync Problem

MFM has a fundamental problem: **there are no inherent byte boundaries**.

```
    Where do bytes start?
    ═════════════════════

    MFM bitstream: ...0 1 0 0 1 0 1 0 1 0 0 0 1 0 0 1 0 1 0 0 1...

    Is this:  [0 1 0 0 1 0 1 0] [1 0 0 0 1 0 0 1] ...  ?
    Or this:  ...[1 0 0 1 0 1 0 1] [0 0 0 1 0 0 1 0] ...  ?
    Or this:  ...[0 0 1 0 1 0 1 0] [0 0 1 0 0 1 0 1] ...  ?

    Without synchronization, we can't decode the data!
```

Even worse, we can't tell clock bits from data bits:

```
    Clock vs Data ambiguity
    ═══════════════════════

    MFM:  ...C D C D C D C D C D C D C D C D...

    We need to know: is this position a Clock or Data slot?
    Getting it wrong shifts ALL subsequent data by one bit!
```

### The Sync Pattern

The sync pattern solves both problems. It consists of:

1. **Preamble**: 12 bytes of `0x00` (96 short pulses)
2. **Sync marks**: 3 bytes of `0xA1` with a **missing clock bit**

Let's trace through this:

```
    Preamble: 12 × 0x00
    ═══════════════════

    0x00 = 0000 0000

    Data:    0   0   0   0   0   0   0   0
    Clock:     1   1   1   1   1   1   1   1   (all clocks are 1!)
    MFM:     0 1 0 1 0 1 0 1 0 1 0 1 0 1 0 1

    Flux:  ──┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌──
             └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘

    All SHORT pulses! 12 bytes × 8 bits = 96 consecutive short intervals.
    This is unmistakable - no other data pattern produces this.
```

Now the sync mark `0xA1`:

```
    Normal 0xA1 encoding:
    ═════════════════════

    0xA1 = 1010 0001

    Data:    1       0       1       0       0       0       0       1
    Clock:     0       0       0       1       1       1       0
                                      ↑       ↑       ↑
    MFM:     1   0   0   0   1   0   0   1   0   1   0   1   0   0   1

    Intervals:   L       M       S   S   S       M
```

But the sync mark has a **missing clock bit**:

```
    Sync 0xA1 encoding (MISSING CLOCK):
    ════════════════════════════════════

    0xA1 = 1010 0001

    Data:    1       0       1       0       0       0       0       1
    Clock:     0       0       0       1       X       1       0
                                              ↑
                                         MISSING!

    MFM:     1   0   0   0   1   0   0   1   0   0   0   1   0   0   1
                                              ↑
    Normal:                          0   1
    Sync:                            0   0   ← Clock bit removed!

    Intervals:   L       M           L       M
                 ↑       ↑           ↑       ↑
              Compare these: LMLM pattern!
```

The complete sync sequence:

```
    Full Sync Sequence: 0x00... + 3×0xA1(sync)
    ═══════════════════════════════════════════

    End of preamble:        ...S S S S S S S S
    Transition to sync:                      M  (0x00→0xA1 boundary)
    First sync 0xA1:        L   M   L   M   S
    Second sync 0xA1:       L   M   L   M   S
    Third sync 0xA1:        L   M   L   M
                            ↑               ↑
                            │               └── Last pulse = DATA bit!
                            │                   (the '1' at end of 0xA1)
                            │
                            └── Pattern LMLM is UNIQUE!

    Full pattern: S S S...S S M L M L M S L M L M S L M L M
                  \_________/ \_____/ \_____/ \_____/
                   preamble    sync1   sync2   sync3
```

**Why LMLM is unique:**

```
    Why can't LMLM occur in normal MFM data?
    ════════════════════════════════════════

    L = 4 bit-cells = 1 0 0 0
    M = 3 bit-cells = 1 0 0

    LMLM = 1000 100 1000 100 = ...1 0 0 0 1 0 0 1 0 0 0 1 0 0...
                                 D C D C D C D C D C D C D C

    For L after M (positions 8-11): we need "1000"
    Position 8 must be D=1 (starts the L interval)
    Position 9 must be C=0 (no clock - previous D=1)
    Position 10 must be D=0
    Position 11 must be C=0 ← But wait!

    If D[10]=0 and D[8]=1, then C[9] = NOT(1 OR 0) = 0 ✓
    If D[10]=0 and D[12]=?, for C[11]=0 we need D[10]=1 OR D[12]=1

    But we said D[10]=0! So D[12] must be 1.
    That means position 12 has a transition → interval is only 3T, not 4T.

    Contradiction! LMLM cannot occur with normal MFM clock rules.
    The ONLY way to get LMLM is to REMOVE a clock bit.
```

Once we see LMLM, we know:
1. We found a sync mark
2. The pulse that ended the pattern is a **DATA bit** (the last '1' of 0xA1)
3. From here, we can decode by tracking position (DATA vs CLOCK)

---

## Disk Format

### Physical Layout

```
    Floppy Disk Layout (HD 1.44MB)
    ══════════════════════════════

    ┌─────────────────────────────────────────────────────────────┐
    │                                                             │
    │    Track 0 ─────────►  ┌───────────────────────────────┐    │
    │                        │                               │    │
    │    Track 1 ─────────►  │  ┌───────────────────────┐    │    │
    │                        │  │                       │    │    │
    │         ...            │  │         ...           │    │    │
    │                        │  │                       │    │    │
    │    Track 79 ───────────────► ┌─────────────────┐  │    │    │
    │                        │  │  │     (center)    │  │    │    │
    │                        │  │  └─────────────────┘  │    │    │
    │                        │  │                       │    │    │
    │                        │  └───────────────────────┘    │    │
    │                        │                               │    │
    │                        └───────────────────────────────┘    │
    │                                                             │
    │    × 2 sides (head 0 and head 1)                            │
    │                                                             │
    └─────────────────────────────────────────────────────────────┘

    80 tracks × 2 sides = 160 "cylinders"
    Each track = 18 sectors of 512 bytes
    Total: 160 × 18 × 512 = 1,474,560 bytes
```

### Track Format

Each track is a continuous stream of flux data. One rotation = 200ms at 300 RPM.

```
    Track Layout (one side of one cylinder)
    ═══════════════════════════════════════

    INDEX pulse marks start of track
    │
    ▼
    ┌────────┬─────────┬─────────┬─────────┬─────────┬────────────┐
    │ GAP 4a │ Sector  │ Sector  │ Sector  │   ...   │   GAP 4b   │
    │ (80×   │    1    │    2    │    3    │         │ (fill to   │
    │  0x4E) │         │         │         │         │  index)    │
    └────────┴─────────┴─────────┴─────────┴─────────┴────────────┘

    GAP 4a:  Post-index gap, allows motor speed to stabilize
    Sectors: 18 sectors with gaps between them
    GAP 4b:  Fills remaining space until next INDEX pulse

    Total raw data per track: ~12,500 bytes of flux data
    (But only 18 × 512 = 9,216 bytes of user data)
```

### Sector Format

Each sector has an address record and a data record:

```
    Sector Structure
    ════════════════

    ┌──────────────────────────────────────────────────────────────────────┐
    │                         ADDRESS RECORD                               │
    ├────────────┬────────────┬────────────────────────────────┬───────────┤
    │  Preamble  │ Sync Marks │           Address Data         │    CRC    │
    │  12× 0x00  │  3× 0xA1   │  0xFE  TRK  SIDE  SEC  SIZE    │  2 bytes  │
    │  (sync)    │  (sync)    │  mark  (0-79)(0-1)(1-18)(0x02) │  CRC-16   │
    └────────────┴────────────┴────────────────────────────────┴───────────┘
                                     │
    ┌────────────────────────────────┘
    │
    │   0xFE = Address mark (identifies this as address record)
    │   TRK  = Track number (0-79)
    │   SIDE = Side/head number (0 or 1)
    │   SEC  = Sector number (1-18, NOT zero-based!)
    │   SIZE = Sector size code: 0=128, 1=256, 2=512, 3=1024 bytes

    ┌────────────┐
    │   GAP 2    │  22 bytes of 0x4E (time for controller to process address)
    └────────────┘

    ┌──────────────────────────────────────────────────────────────────────┐
    │                          DATA RECORD                                 │
    ├────────────┬────────────┬────────────────────────────────┬───────────┤
    │  Preamble  │ Sync Marks │         Sector Data            │    CRC    │
    │  12× 0x00  │  3× 0xA1   │  0xFB + 512 bytes of data      │  2 bytes  │
    │  (sync)    │  (sync)    │  mark                          │  CRC-16   │
    └────────────┴────────────┴────────────────────────────────┴───────────┘
                                     │
    │   0xFB = Data mark (normal data)
    │   0xFA = Data mark (deleted data - rarely used)

    ┌────────────┐
    │   GAP 3    │  54+ bytes of 0x4E (between sectors)
    └────────────┘
```

The CRC-16 calculation includes the three `0xA1` sync marks:

```c
// CRC includes sync marks!
crc = 0xFFFF;                    // Initial value
crc = crc16_update(crc, 0xA1);   // First sync
crc = crc16_update(crc, 0xA1);   // Second sync
crc = crc16_update(crc, 0xA1);   // Third sync
crc = crc16_update(crc, 0xFE);   // Address/data mark
// ... then the actual data bytes
```

---

## Implementation

### Architecture

```
    Code Organization
    ═════════════════

    src/
    ├── flux_read.pio      PIO program for reading flux pulses
    ├── flux_write.pio     PIO program for writing flux pulses
    ├── mfm_decode.c/h     MFM decoder state machine
    ├── mfm_encode.c/h     MFM encoder
    ├── crc.h              CRC-16 lookup table
    ├── floppy.c/h         Drive control, sector read/write
    ├── fat12.c/h          FAT12 filesystem implementation
    ├── f12.c/h            High-level file API with caching
    └── lru.c/h            LRU cache for sectors

    examples/
    └── cli.c              Interactive demo application
```

### PIO Programs

The RP2040's PIO provides cycle-accurate timing essential for floppy operations.

#### flux_read.pio - Measuring Pulse Intervals

```
    PIO Clock Configuration
    ═══════════════════════

    System clock: 125 MHz (default)
    PIO clock: 72 MHz (125 MHz ÷ 1.736)
    Loop iterations: 3 instructions per loop
    Effective sample rate: 72 MHz ÷ 3 = 24 MHz
    Resolution: 1 ÷ 24 MHz = 41.67 ns per count

    At 500 kbps MFM (HD floppy):
      2T (short)  = 2 μs = 48 counts
      3T (medium) = 3 μs = 72 counts
      4T (long)   = 4 μs = 96 counts
```

The PIO program:

```
    flux_read PIO Program
    ═════════════════════

    X register: 15-bit down-counter (time since last transition)

    .wrap_target
    wait_high:
        jmp x-- wait_high_next    ; [1] Decrement counter
    wait_high_next:
        jmp pin wait_low          ; [2] If pin HIGH, go wait for LOW
        jmp wait_high             ; [3] Pin still LOW, keep waiting

    wait_low:
        jmp x-- wait_low_next     ; [1] Decrement counter
    wait_low_next:
        jmp pin wait_low [1]      ; [2+1] If pin still HIGH, wait (extra delay)
        ; Falling edge detected!
        in pins, 1                ; [1] Capture INDEX pin state
        in x, 15                  ; [1] Push counter value (15 bits)
        jmp x-- wait_high         ; [1] Reset X to 0xFFFF (wraps from 0)
    .wrap


    State Diagram:
    ──────────────

         ┌──────────────────────┐
         │                      │
         ▼                      │ pin=LOW
    ┌─────────┐                 │
    │wait_high│─────────────────┘
    │  (x--)  │
    └────┬────┘
         │ pin=HIGH
         ▼
    ┌─────────┐    pin=HIGH
    │wait_low │◄───────────┐
    │  (x--)  │            │
    └────┬────┘────────────┘
         │ pin=LOW (falling edge!)
         ▼
    ┌─────────────────┐
    │ in pins, 1      │  Capture index pin
    │ in x, 15        │  Push counter to FIFO
    │ jmp x-- (reset) │  X wraps 0→0xFFFF
    └────────┬────────┘
             │
             └──► back to wait_high
```

The output is packed into 32-bit words (two 16-bit samples):

```
    FIFO Output Format
    ══════════════════

    32-bit word from FIFO:
    ┌─────────────────────────────────┬─────────────────────────────────┐
    │  Bits 31-16: Second sample      │  Bits 15-0: First sample        │
    ├─────────────────┬───────────────┼─────────────────┬───────────────┤
    │  Bit 31: INDEX  │ Bits 30-16:   │  Bit 15: INDEX  │ Bits 14-0:    │
    │  pin state      │ Counter value │  pin state      │ Counter value │
    └─────────────────┴───────────────┴─────────────────┴───────────────┘

    The counter counts DOWN from 0x7FFF.
    Delta time = previous_count - current_count (with wrap handling)
```

**The INDEX pin** is captured with each pulse. This serves two purposes:

1. **Rotation counting**: The index pin pulses low once per disk rotation (every 200ms). By watching for transitions in the index bit, we can count rotations and timeout if a sector isn't found.

2. **Track alignment**: When writing, we wait for the index pulse to start writing at a consistent position.

```c
// Reading with rotation timeout:
for (int ix_edges = 0; ix_edges < 10;) {  // 5 rotations max
    uint16_t value = flux_read_wait(f);
    uint8_t ix = value & 1;        // INDEX pin state (LSB)
    uint16_t cnt = value >> 1;     // Counter value

    if (ix != ix_prev) ix_edges++; // Count index transitions
    ix_prev = ix;

    // Process the pulse...
}
```

#### flux_write.pio - Generating Pulse Intervals

```
    flux_write PIO Program
    ══════════════════════

    .wrap_target
        pull block              ; [1] Get timing value from FIFO (blocks if empty)
        out x, 8                ; [1] Move 8 bits to X register

        set pins, 0             ; [1] Drive WRITE_DATA LOW (flux transition!)
        nop [13]                ; [14] Hold LOW for 14 cycles (~580 ns)
        set pins, 1             ; [1] Drive WRITE_DATA HIGH

    wait_loop:
        jmp x-- wait_loop       ; [1×X] Wait for timing countdown
    .wrap

    Timing:
    ═══════
    Overhead per pulse: pull(1) + out(1) + set(1) + nop[13](14) + set(1) + jmp(1) = 19 cycles

    For a 2μs interval (48 cycles at 24 MHz):
      X = 48 - 19 = 29 loop iterations

    Input values sent to PIO:
      Short  (2T): 29  (48 - 19)
      Medium (3T): 53  (72 - 19)
      Long   (4T): 77  (96 - 19)
```

### MFM Decoder

The decoder is a state machine that processes pulse intervals:

```
    MFM Decoder State Machine
    ═════════════════════════

                      ┌──────────────────────────────────────┐
                      │              HUNT                    │
                      │   Looking for preamble               │
                      │   (count consecutive short pulses)   │
                      └──────────────────┬───────────────────┘
                                         │ ≥60 short pulses followed
                                         │ by non-short pulse
                                         ▼
                      ┌──────────────────────────────────────┐
                      │            SYNCING                   │
                      │   Matching sync pattern              │
                      │   M L M L M S L M L M S L M L M      │
                      └──────────────────┬───────────────────┘
                                         │ Pattern matched!
                                         │ (15 pulses verified)
                                         ▼
              ┌──────────────────────────────────────────────────────┐
              │                       DATA                           │
              │   Current position is a DATA bit                     │
              │   ┌────────────────────────────────────────────────┐ │
              │   │ Short(S):  push bit 1, stay at DATA            │ │
              │   │ Medium(M): push bits 00, go to CLOCK           │ │
              │   │ Long(L):   push bits 01, stay at DATA          │ │
              │   └────────────────────────────────────────────────┘ │
              └───────────────────────┬──────────────────────────────┘
                                      │ M (medium pulse)
                                      ▼
              ┌──────────────────────────────────────────────────────┐
              │                      CLOCK                           │
              │   Current position is a CLOCK bit                    │
              │   ┌────────────────────────────────────────────────┐ │
              │   │ Short(S):  push bit 0, stay at CLOCK           │ │
              │   │ Medium(M): push bit 1, go to DATA              │ │
              │   │ Long(L):   ERROR! Reset to HUNT                │ │
              │   └────────────────────────────────────────────────┘ │
              └──────────────────────────────────────────────────────┘
```

**Why Long at CLOCK is an error:**

```
    Long interval = 4 bit-cells = "1 0 0 0"

    If we're at a CLOCK position, the next 4 cells are:  C D C D
    For interval "1000":
      - C1 = 1 (transition here)
      - D1 = 0
      - C2 = 0
      - D2 = 0

    But C2 = NOR(D1, D2) = NOR(0, 0) = 1, not 0!
    Contradiction → This pattern can't occur in valid MFM
    (Unless it's a sync mark, but we already found sync)
```

The decoder also tracks record boundaries:

```c
// After sync, first byte tells us record type:
if (m->buf_pos == 1 && m->bytes_expected == 0) {
    uint8_t mark = m->buf[0];
    if (mark == 0xFE) {
        // Address record: expect 7 bytes total
        m->bytes_expected = 7;  // FE + track + side + sector + size + CRC(2)
    } else if (mark == 0xFB || mark == 0xFA) {
        // Data record: expect 515 bytes total (mark + 512 data + 2 CRC)
        m->bytes_expected = 515;
    }
}
```

### MFM Encoder

The encoder converts data bytes to pulse timings:

```
    MFM Encoder
    ═══════════

    State:
      - prev_bit: last data bit (for clock calculation)
      - pending_cells: half-cells since last transition

    For each bit:
    ┌─────────────────────────────────────────────────────────────┐
    │  clock_bit = (prev_bit == 0 && data_bit == 0) ? 1 : 0       │
    │                                                             │
    │  if (clock_bit == 1):                                       │
    │      emit_pulse(pending_cells)   // Transition on clock     │
    │      pending_cells = 0                                      │
    │  else:                                                      │
    │      pending_cells++              // No clock transition    │
    │                                                             │
    │  if (data_bit == 1):                                        │
    │      emit_pulse(pending_cells)   // Transition on data      │
    │      pending_cells = 0                                      │
    │  else:                                                      │
    │      pending_cells++              // No data transition     │
    │                                                             │
    │  prev_bit = data_bit                                        │
    └─────────────────────────────────────────────────────────────┘

    emit_pulse(cells):
      cells ≤ 1  → Short  (2T)
      cells == 2 → Medium (3T)
      cells ≥ 3  → Long   (4T)
```

Sync marks are special-cased because they violate normal encoding:

```c
void mfm_encode_sync(mfm_encode_t *e) {
    // Preamble: 12 bytes of 0x00 (normal encoding)
    uint8_t preamble[12] = {0};
    mfm_encode_bytes(e, preamble, 12);

    // Sync marks: hardcoded pulse pattern (can't be generated normally!)
    static const uint8_t sync_pulses[] = {
        MFM_PULSE_MEDIUM, MFM_PULSE_LONG, MFM_PULSE_MEDIUM, MFM_PULSE_LONG, MFM_PULSE_MEDIUM,
        MFM_PULSE_SHORT,
        MFM_PULSE_LONG, MFM_PULSE_MEDIUM, MFM_PULSE_LONG, MFM_PULSE_MEDIUM,
        MFM_PULSE_SHORT,
        MFM_PULSE_LONG, MFM_PULSE_MEDIUM, MFM_PULSE_LONG, MFM_PULSE_MEDIUM
    };
    // M L M L M S L M L M S L M L M
    //  \_____/    \_____/    \_____/
    //   sync1      sync2      sync3

    for (int i = 0; i < 15; i++) {
        mfm_encode_pulse(e, sync_pulses[i]);
    }

    // After sync, we're at a DATA position (last bit of 0xA1 = 1)
    e->prev_bit = 1;
    e->pending_cells = 0;
}
```

### Floppy Driver

The `floppy.c` module handles physical drive control:

```
    Drive Control Sequences
    ═══════════════════════

    Power-on / Select Drive:
    ────────────────────────
    1. Pull DRIVE_SELECT low
    2. Wait 10ms for logic to stabilize
    3. Pull MOTOR_ENABLE low
    4. Wait 750ms for motor to reach speed
    5. Verify by watching INDEX pulses (should see ~5 per second)


    Seek to Track:
    ──────────────
    1. Set DIRECTION (low = toward center, high = toward edge)
    2. Wait 10μs
    3. Pulse STEP low for 10μs, then high
    4. Wait 10ms for head to settle
    5. Repeat for each track to move


    Seek to Track 0:
    ────────────────
    1. Set DIRECTION high (toward edge)
    2. Step up to 90 times, checking TRACK0 pin
    3. When TRACK0 goes low, we're at track 0


    Read Sector:
    ────────────
    1. Seek to track
    2. Set SIDE_SELECT (high = side 0, low = side 1)
    3. Start PIO read state machine
    4. Feed pulses to MFM decoder
    5. Wait for address record matching desired sector
    6. Continue decoding to get data record
    7. Verify CRC


    Write Track:
    ────────────
    1. Read any sectors we need to preserve (can't write partial tracks!)
    2. Encode entire track to flux buffer
    3. Seek to track, set side
    4. Wait for INDEX pulse (start of track)
    5. Pull WRITE_GATE low
    6. Start PIO write state machine, feed encoded pulses
    7. Pull WRITE_GATE high when done
```

**Why write entire tracks?**

```
    Partial Track Writing Problem
    ═════════════════════════════

    Floppies have no absolute position within a track. The INDEX pulse
    marks the start, but there's no way to say "start writing at byte 1234".

    If we try to overwrite just sector 5:

    ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
    │ S1  │ S2  │ S3  │ S4  │ S5  │ S6  │ S7  │ ... │  Original
    └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
                              │
                              ▼ Try to overwrite S5
    ┌─────┬─────┬─────┬─────┬──────┬─────┬─────┬────┐
    │ S1  │ S2  │ S3  │ S4  │ S5'  │ S6  │ S7  │... │  Attempted
    └─────┴─────┴─────┴─────┴──────┴─────┴─────┴────┘
                              │    │
                              │    └─ But we can't hit exact byte boundary!
                              │       Write might start a few bits early/late.
                              │
                              └─ Start position uncertain (motor speed varies)

    Result: S5 corrupted, possibly S4 or S6 too!

    Solution: Read entire track, modify desired sectors, write entire track.
```

### FAT12 Filesystem

Standard PC floppy format:

```
    FAT12 Disk Layout (1.44MB)
    ══════════════════════════

    LBA    Content              CHS
    ───    ─────────────────    ─────────────
      0    Boot sector          C0 H0 S1
      1    FAT #1 (sector 1)    C0 H0 S2
     ...   FAT #1 (9 sectors)
      9    FAT #1 (sector 9)    C0 H0 S10
     10    FAT #2 (sector 1)    C0 H0 S11
     ...   FAT #2 (9 sectors)
     18    FAT #2 (sector 9)    C0 H1 S1
     19    Root directory       C0 H1 S2
     ...   Root dir (14 sectors, 224 entries)
     32    Root directory end   C0 H1 S15
     33    Data area (cluster 2) C0 H1 S16
     34    Data area (cluster 3) C0 H1 S17
     ...
   2879    Last sector          C79 H1 S18

    LBA to CHS conversion:
      cylinder = LBA / (heads × sectors_per_track)
      temp     = LBA % (heads × sectors_per_track)
      head     = temp / sectors_per_track
      sector   = (temp % sectors_per_track) + 1   // Sectors are 1-based!
```

**12-bit FAT entries:**

```
    FAT12 Entry Packing
    ═══════════════════

    FAT12 uses 12 bits per cluster entry, packed into bytes:

    Cluster:     0       1       2        3
    Bits:     [11:0]  [11:0]  [11:0]   [11:0]
               │        │        │        │
               ▼        ▼        ▼        ▼
    Bytes:  [  AB  ] [  CD  ] [  EF  ] [  GH  ] [  IJ  ] [  KL  ]
            └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘

    Cluster 0: bits  0-11 = low 8 bits of byte 0 + low 4 bits of byte 1
    Cluster 1: bits 12-23 = high 4 bits of byte 1 + all 8 bits of byte 2
    Cluster 2: bits 24-35 = low 8 bits of byte 3 + low 4 bits of byte 4
    ...

    Code:
    ┌────────────────────────────────────────────────────────────┐
    │  fat_offset = cluster + (cluster / 2);  // 1.5 bytes each  │
    │  sector = fat_start + (fat_offset / 512);                  │
    │  offset = fat_offset % 512;                                │
    │  word = sector[offset] | (sector[offset+1] << 8);          │
    │                                                            │
    │  if (cluster & 1)        // Odd cluster                    │
    │      value = word >> 4;  // High 12 bits                   │
    │  else                    // Even cluster                   │
    │      value = word & 0x0FFF;  // Low 12 bits                │
    └────────────────────────────────────────────────────────────┘

    Special values:
      0x000        = Free cluster
      0x002-0xFEF  = Next cluster in chain
      0xFF0-0xFF6  = Reserved
      0xFF7        = Bad cluster
      0xFF8-0xFFF  = End of chain (EOF)
```

**Write batching:**

```c
// Can't write single sectors - must batch by track
typedef struct {
    uint16_t lbas[36];              // Up to 2 tracks worth
    uint8_t data[36][SECTOR_SIZE];
    uint8_t count;
} fat12_write_batch_t;

// When batch is full or on flush:
//   1. Group sectors by track
//   2. For each track with dirty sectors:
//      a. Read missing sectors from disk
//      b. Write entire track
```

### F12 API

High-level POSIX-like interface:

```c
// Mounting
f12_t fs;
f12_io_t io = {
    .read = floppy_io_read,
    .write = floppy_io_write,
    .disk_changed = floppy_io_disk_changed,
    .write_protected = floppy_io_write_protected,
    .ctx = &floppy,
};
f12_mount(&fs, io);

// File operations
f12_file_t *f = f12_open(&fs, "FILE.TXT", "r");  // "r", "w", "a"
int n = f12_read(f, buf, len);
int n = f12_write(f, buf, len);
f12_close(f);

// Directory listing
f12_dir_t dir;
f12_stat_t stat;
f12_opendir(&fs, "/", &dir);
while (f12_readdir(&dir, &stat) == F12_OK) {
    printf("%s %lu\n", stat.name, stat.size);
}
f12_closedir(&dir);
```

**LRU sector cache:**

```
    LRU Cache (36 sectors = 2 tracks)
    ══════════════════════════════════

    FAT operations repeatedly access the same sectors:
    - Boot sector (BPB parameters)
    - FAT sectors (cluster chains)
    - Root directory (file lookup)

    Cache structure:
    ┌─────────────────────────────────────────────────────────────┐
    │  Most Recently Used                    Least Recently Used  │
    │        │                                        │           │
    │        ▼                                        ▼           │
    │    ┌──────┐   ┌──────┐   ┌──────┐         ┌──────┐          │
    │    │ C0H0 │◄─►│ C0H0 │◄─►│ C0H1 │◄─► . ◄─►│ C1H0 │          │
    │    │ S1   │   │ S2   │   │ S1   │         │ S5   │          │
    │    └──────┘   └──────┘   └──────┘         └──────┘          │
    │                                              ▲              │
    │                                              │              │
    │                                    Evicted when cache full  │
    └─────────────────────────────────────────────────────────────┘

    Key = (track << 16) | (side << 8) | sector_n

    On read: check cache first, read from disk on miss
    On write: write through to disk, update cache
    On disk change: invalidate entire cache
```

---

## Usage

### Basic Example

```c
#include "floppy.h"
#include "f12.h"

int main(void) {
    stdio_init_all();

    // Configure pin mapping
    floppy_t floppy = {
        .pins = {
            .index         = 2,
            .track0        = 3,
            .write_protect = 4,
            .read_data     = 5,   // Add 4.7kΩ pull-up to 3.3V!
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

    // Initialize hardware
    floppy_init(&floppy);
    floppy_set_density(&floppy, true);  // HD mode (1.44MB)

    // Mount filesystem
    f12_t fs;
    f12_io_t io = {
        .read = floppy_io_read,
        .write = floppy_io_write,
        .disk_changed = floppy_io_disk_changed,
        .write_protected = floppy_io_write_protected,
        .ctx = &floppy,
    };

    if (f12_mount(&fs, io) != F12_OK) {
        printf("Mount failed!\n");
        return 1;
    }

    // List files
    f12_dir_t dir;
    f12_stat_t stat;
    f12_opendir(&fs, "/", &dir);
    while (f12_readdir(&dir, &stat) == F12_OK) {
        printf("%-12s %8lu bytes\n", stat.name, stat.size);
    }
    f12_closedir(&dir);

    // Read a file
    f12_file_t *file = f12_open(&fs, "README.TXT", "r");
    if (file) {
        char buf[256];
        int n;
        while ((n = f12_read(file, buf, sizeof(buf)-1)) > 0) {
            buf[n] = '\0';
            printf("%s", buf);
        }
        f12_close(file);
    }

    // Write a file
    file = f12_open(&fs, "HELLO.TXT", "w");
    if (file) {
        const char *msg = "Hello from Pico!\r\n";
        f12_write(file, msg, strlen(msg));
        f12_close(file);
    }

    // Format a disk
    f12_format(&fs, "MYDISK", false);  // false = quick format

    f12_unmount(&fs);
    return 0;
}
```

### Auto Motor Management

The driver automatically manages motor state:

```
    Auto Motor Timeline
    ═══════════════════

    Time ──────────────────────────────────────────────────────────►

    I/O request
        │
        ▼
    ┌───────────────────┐
    │ Motor spins up    │ (750ms delay on first access)
    │ if not running    │
    └───────────────────┘
        │
        ▼
    ┌───────────────────┐
    │ Perform I/O       │
    └───────────────────┘
        │
        ▼
    ┌───────────────────────────────────────────────────────┐
    │                    Idle timer                         │
    │                   (20 seconds)                        │
    └───────────────────────────────────────────────────────┘
        │                                                   │
        │ More I/O?                              Timer expires
        │                                                   │
        ▼                                                   ▼
    ┌───────────────────┐                       ┌───────────────────┐
    │ Reset timer       │                       │ Motor spins down  │
    │ Continue          │                       │ Drive deselected  │
    └───────────────────┘                       └───────────────────┘
```

---

## Design Decisions

### Why PIO instead of CPU bit-banging?

MFM requires ~42ns timing resolution (24 MHz). The RP2040 at 125 MHz has 8ns clock cycles, but:

- **Interrupt latency**: 12+ cycles minimum
- **Cache misses**: Unpredictable delays
- **USB/stdio interrupts**: Can't be fully disabled

PIO runs independently with deterministic timing, immune to CPU interruptions.

### Why track-at-a-time writes?

Floppy disks have no absolute position markers within a track. The only reference point is the INDEX pulse (once per rotation). Attempting to overwrite a single sector risks:

1. **Position uncertainty**: Motor speed varies ±2%
2. **Bit slip**: Starting/stopping write at wrong bit boundary
3. **Splice artifacts**: Transition from old to new data creates noise

Writing complete tracks from INDEX to INDEX eliminates these issues.

### Why LRU cache?

FAT12 operations are read-heavy and repetitive:
- Reading the FAT to follow cluster chains
- Scanning root directory for files
- Re-reading boot sector for BPB values

A 36-sector cache (18KB) dramatically reduces physical I/O while fitting easily in the RP2040's 264KB RAM.

### Why threshold-based pulse classification?

Real hardware has timing variation:
- Motor speed: 300 RPM ±1.5%
- Media quality: Magnetic domains vary
- Temperature: Affects timing circuits

Rather than expecting exact 2.00/3.00/4.00 μs intervals, we use midpoint thresholds:
- Short: ≤2.4 μs (57 counts)
- Medium: ≤3.4 μs (82 counts)
- Long: >3.4 μs

This provides ~20% tolerance on each interval.

---

## References

- [floppy.cafe](https://floppy.cafe) - Excellent MFM encoding and floppy documentation (Josh Cole)
- [Peter Schranz's MFM Reader](https://www.pdp-11.org/projects.html) - AVR assembler implementation
- [Adafruit Floppy](https://github.com/adafruit/Adafruit_Floppy) - Arduino/RP2040 floppy library
- Samsung SFD321B datasheet - Floppy drive pinout and timing specifications
- [Floppy Disk Formats](http://www.isdaman.com/alsos/hardware/fdc/floppy.htm) - Comprehensive format reference
- Microsoft FAT12/16/32 File System Specification

---

## License

MIT
