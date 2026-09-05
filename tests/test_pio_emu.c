#include "test.h"
#include "pio_emu.h"
#include "flux_read.pio.h"
#include "flux_write.pio.h"
#include "../src/mfm.h"
#include "../src/floppy.h"
#include <string.h>

static void load_read_program(pio_emu_t *emu) {
  pio_emu_init(emu);
  pio_emu_load(emu, flux_read_program_instructions,
               sizeof(flux_read_program_instructions) / sizeof(flux_read_program_instructions[0]),
               flux_read_wrap_target, flux_read_wrap);
  emu->in_shift_right = true;
  emu->autopush_threshold = 32;
  emu->x = 0;
  emu->jmp_pin = true;
}

static void load_write_program(pio_emu_t *emu) {
  pio_emu_init(emu);
  pio_emu_load(emu, flux_write_program_instructions,
               sizeof(flux_write_program_instructions) /
                   sizeof(flux_write_program_instructions[0]),
               flux_write_wrap_target, flux_write_wrap);
  emu->out_shift_right = true;
  emu->autopull_threshold = 0;
  emu->set_pins = 1;
}

TEST(test_generated_write_program_has_one_pull) {
  unsigned pulls = 0;
  unsigned outs = 0;
  for (size_t i = 0;
       i < sizeof(flux_write_program_instructions) /
               sizeof(flux_write_program_instructions[0]);
       i++) {
    uint16_t instruction = flux_write_program_instructions[i];
    unsigned op = instruction >> 13;
    if (op == PIO_OP_PUSH_PULL && ((instruction >> 7) & 1u)) pulls++;
    if (op == PIO_OP_OUT) outs++;
  }
  ASSERT_EQ(pulls, 1);
  ASSERT_EQ(outs, 1);
}

TEST(test_generated_write_edges_have_exact_intervals) {
  pio_emu_t emu;
  load_write_program(&emu);
  static const uint8_t pulses[] = {
      MFM_PULSE_SHORT, MFM_PULSE_MEDIUM, MFM_PULSE_LONG,
      MFM_PULSE_SHORT, MFM_PULSE_LONG,
  };
  for (size_t i = 0; i < sizeof(pulses); i++) pio_emu_tx_put(&emu, pulses[i] - FLOPPY_WRITE_PIO_OVERHEAD);
  uint64_t falling[sizeof(pulses)];
  size_t count = 0;
  bool previous = emu.set_pins;
  for (uint32_t cycle = 0; cycle < 10000 && count < sizeof(pulses); cycle++) {
    pio_emu_step(&emu);
    bool current = emu.set_pins;
    if (previous && !current) falling[count++] = emu.cycle_count;
    previous = current;
  }
  ASSERT_EQ(count, sizeof(pulses));
  for (size_t i = 1; i < count; i++) {
    ASSERT_EQ(falling[i] - falling[i - 1], pulses[i - 1]);
  }
}

TEST(test_generated_write_starvation_stalls_high) {
  pio_emu_t emu;
  load_write_program(&emu);
  pio_emu_tx_put(&emu, MFM_PULSE_SHORT - FLOPPY_WRITE_PIO_OVERHEAD);
  bool saw_falling = false;
  bool previous = emu.set_pins;
  for (unsigned cycle = 0; cycle < 1000 && !emu.stalled; cycle++) {
    pio_emu_step(&emu);
    bool current = emu.set_pins;
    if (previous && !current) saw_falling = true;
    previous = current;
  }
  ASSERT(saw_falling);
  ASSERT(emu.stalled);
  ASSERT_EQ(emu.set_pins, 1);
  uint64_t stalled_at = emu.cycle_count;
  for (unsigned i = 0; i < 100; i++) pio_emu_step(&emu);
  ASSERT_EQ(emu.set_pins, 1);
  ASSERT_EQ(emu.tx_count, 0);
  ASSERT_EQ(emu.cycle_count, stalled_at + 100);
  pio_emu_tx_put(&emu, MFM_PULSE_MEDIUM - FLOPPY_WRITE_PIO_OVERHEAD);
  bool resumed = false;
  previous = emu.set_pins;
  for (unsigned i = 0; i < 1000 && !resumed; i++) {
    pio_emu_step(&emu);
    bool current = emu.set_pins;
    resumed = previous && !current;
    previous = current;
  }
  ASSERT(resumed);
}

TEST(test_generated_programs_roundtrip_a_precompensated_track) {
  static uint8_t pulses[200000];
  track_t track = {.cylinder = 79, .head = 1, .valid = DISK_TRACK_VALID};
  for (size_t sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    for (size_t i = 0; i < DISK_SECTOR_SIZE; i++) {
      track.data[sector][i] = (uint8_t)(i * 29u + sector * 7u);
    }
  }
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_track(&encoder, &track);
  ASSERT(!encoder.stopped);

  pio_emu_t writer;
  pio_emu_t reader;
  load_write_program(&writer);
  load_read_program(&reader);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  size_t queued = 0;
  bool primed = false;
  uint16_t previous = 0;
  uint32_t seen = 0;
  for (uint64_t cycles = 0; cycles < 6000000u && seen != DISK_TRACK_VALID; cycles++) {
    if (queued < encoder.pos && !pio_emu_tx_full(&writer)) {
      pio_emu_tx_put(&writer, pulses[queued++] - FLOPPY_WRITE_PIO_OVERHEAD);
    }
    ASSERT(pio_emu_step(&writer));
    reader.jmp_pin = writer.set_pins != 0;
    for (unsigned tick = 0; tick < MFM_READ_PIO_HZ / MFM_WRITE_PIO_HZ; tick++) {
      ASSERT(pio_emu_step(&reader));
      if (pio_emu_rx_empty(&reader)) continue;
      uint32_t word = pio_emu_rx_get(&reader);
      for (unsigned half = 0; half < 2; half++) {
        uint16_t sample = (uint16_t)((word >> (half * 16u + 1u)) & 0x7FFFu);
        uint16_t interval = (uint16_t)((previous - sample) & 0x7FFFu);
        previous = sample;
        if (!primed) {
          primed = true;
          continue;
        }
        if (!mfm_feed(&decoder, interval, &sector)) continue;
        ASSERT_EQ(sector.cylinder, track.cylinder);
        ASSERT_EQ(sector.head, track.head);
        ASSERT((seen & (1u << sector.sector)) == 0);
        ASSERT_MEM_EQ(sector.data, track.data[sector.sector], DISK_SECTOR_SIZE);
        seen |= 1u << sector.sector;
      }
    }
  }
  ASSERT_EQ(seen, DISK_TRACK_VALID);
  ASSERT_EQ(decoder.crc_errors, 0);
  ASSERT_EQ(decoder.format_errors, 0);
}

TEST(test_generated_read_program_captures_flux) {
  pio_emu_t emu;
  load_read_program(&emu);
  uint16_t deltas[] = {48, 72, 48, 96, 48, 48, 72};
  size_t delta = 0;
  uint32_t remaining = deltas[0] * 3u;
  unsigned samples = 0;
  uint32_t words[2] = {0};
  for (uint64_t cycle = 0; cycle < 100000 && samples < 4; cycle++) {
    remaining--;
    if (remaining == 12) {
      emu.jmp_pin = false;
    } else if (remaining == 0) {
      emu.jmp_pin = true;
      delta++;
      remaining = delta < sizeof(deltas) / sizeof(deltas[0])
                      ? deltas[delta] * 3u
                      : UINT32_MAX;
    }
    emu.pin_values = delta < 2;
    pio_emu_step(&emu);
    if (!pio_emu_rx_empty(&emu)) {
      words[samples / 2u] = pio_emu_rx_get(&emu);
      samples += 2;
    }
  }
  ASSERT_EQ(samples, 4);
  uint16_t packed[] = {
      (uint16_t)(words[0] & 0xFFFFu), (uint16_t)(words[0] >> 16),
      (uint16_t)(words[1] & 0xFFFFu), (uint16_t)(words[1] >> 16),
  };
  ASSERT_EQ(packed[0] & 1u, 1);
  ASSERT_EQ(packed[1] & 1u, 1);
  ASSERT_EQ(packed[2] & 1u, 0);
  ASSERT_EQ(packed[3] & 1u, 0);
  ASSERT_EQ((packed[0] >> 1) - (packed[1] >> 1), 72);
  ASSERT_EQ((packed[1] >> 1) - (packed[2] >> 1), 48);
  ASSERT_EQ((packed[2] >> 1) - (packed[3] >> 1), 96);
}

TEST(test_generated_read_timing_has_no_cumulative_bias) {
  static const uint16_t periods[] = {114, 144, 145, 171, 216, 287, 357};
  static const uint8_t widths[] = {3, 7, 14, 30};
  for (size_t p = 0; p < sizeof(periods) / sizeof(periods[0]); p++) {
    for (size_t w = 0; w < sizeof(widths); w++) {
      for (unsigned phase = 0; phase < 3; phase++) {
        pio_emu_t emu;
        load_read_program(&emu);
        unsigned samples = 0;
        uint16_t previous = 0;
        uint32_t measured = 0;
        uint32_t period = periods[p];
        uint32_t start = 24u + phase;
        for (uint32_t cycle = 0; cycle < period * 1100u && samples < 1000; cycle++) {
          emu.jmp_pin = cycle < start || (cycle - start) % period >= widths[w];
          ASSERT(pio_emu_step(&emu));
          if (pio_emu_rx_empty(&emu)) continue;
          uint32_t word = pio_emu_rx_get(&emu);
          for (unsigned half = 0; half < 2; half++) {
            uint16_t current = (uint16_t)((word >> (half * 16u + 1u)) & 0x7FFFu);
            if (samples != 0) {
              uint16_t interval = (uint16_t)((previous - current) & 0x7FFFu);
              ASSERT(interval >= period / 3u);
              ASSERT(interval <= (period + 2u) / 3u);
              measured += interval;
            }
            previous = current;
            samples++;
          }
        }
        ASSERT_EQ(samples, 1000);
        uint32_t expected_cycles = (samples - 1u) * period;
        ASSERT(measured * 3u + 2u >= expected_cycles);
        ASSERT(measured * 3u <= expected_cycles + 2u);
      }
    }
  }
}

TEST(test_generated_read_counter_wraps_from_zero) {
  pio_emu_t emu;
  load_read_program(&emu);
  ASSERT_EQ(emu.x, 0);
  ASSERT_EQ(emu.pc, flux_read_wrap_target);
  pio_emu_step(&emu);
  ASSERT_EQ(emu.x, UINT32_MAX);
  ASSERT_EQ(emu.pc, flux_read_wrap_target + 1u);
}

TEST(test_emulator_rejects_invalid_programs_and_handles_32_bit_shifts) {
  pio_emu_t emu;
  pio_emu_init(&emu);
  uint16_t instruction = (uint16_t)((PIO_OP_IN << 13u) | (IN_X << 5u));
  ASSERT(pio_emu_load(&emu, &instruction, 1, 0, 0));
  emu.x = 0x89ABCDEFu;
  ASSERT(pio_emu_step(&emu));
  ASSERT_EQ(emu.isr, 0x89ABCDEFu);

  pio_emu_init(&emu);
  instruction = (uint16_t)((PIO_OP_OUT << 13u) | (OUT_X << 5u));
  ASSERT(pio_emu_load(&emu, &instruction, 1, 0, 0));
  emu.osr = 0x76543210u;
  ASSERT(pio_emu_step(&emu));
  ASSERT_EQ(emu.x, 0x76543210u);

  uint16_t oversized[PIO_EMU_MAX_PROGRAM + 1u] = {0};
  ASSERT(!pio_emu_load(&emu, oversized, PIO_EMU_MAX_PROGRAM + 1u, 0, 0));
  ASSERT(!pio_emu_load(NULL, &instruction, 1, 0, 0));
}

int main(void) {
  printf("=== Generated PIO Emulator Tests ===\n\n");
  RUN_TEST(test_generated_write_program_has_one_pull);
  RUN_TEST(test_generated_write_edges_have_exact_intervals);
  RUN_TEST(test_generated_write_starvation_stalls_high);
  RUN_TEST(test_generated_programs_roundtrip_a_precompensated_track);
  RUN_TEST(test_generated_read_program_captures_flux);
  RUN_TEST(test_generated_read_timing_has_no_cumulative_bias);
  RUN_TEST(test_generated_read_counter_wraps_from_zero);
  RUN_TEST(test_emulator_rejects_invalid_programs_and_handles_32_bit_shifts);
  TEST_RESULTS();
}
