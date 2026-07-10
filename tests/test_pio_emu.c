#include "test.h"
#include "pio_emu.h"
#include "flux_read.pio.h"
#include "flux_write.pio.h"
#include "../src/mfm_decode.h"
#include "../src/mfm_encode.h"
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
  for (size_t i = 0; i < sizeof(pulses); i++) pio_emu_tx_put(&emu, pulses[i]);
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
    ASSERT_EQ(falling[i] - falling[i - 1], pulses[i - 1] + MFM_PIO_OVERHEAD);
  }
}

TEST(test_generated_write_starvation_stalls_high) {
  pio_emu_t emu;
  load_write_program(&emu);
  pio_emu_tx_put(&emu, MFM_PULSE_SHORT);
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
  pio_emu_tx_put(&emu, MFM_PULSE_MEDIUM);
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

TEST(test_generated_write_mfm_roundtrip) {
  uint8_t pulses[16384];
  uint8_t data[DISK_SECTOR_SIZE];
  for (size_t i = 0; i < sizeof(data); i++) {
    data[i] = (uint8_t)(i * 29u + 7u);
  }
  mfm_encode_t encoder;
  mfm_encode_init(&encoder, pulses, sizeof(pulses));
  mfm_encode_gap(&encoder, 80);
  mfm_encode_sector(&encoder, 7, 1, 4, data);
  mfm_encode_gap(&encoder, 54);

  pio_emu_t emu;
  load_write_program(&emu);
  mfm_t decoder;
  mfm_sector_t sector;
  mfm_init(&decoder);
  size_t queued = 0;
  bool previous_pin = emu.set_pins;
  bool have_edge = false;
  uint64_t previous_edge = 0;
  bool found = false;
  for (uint64_t cycles = 0; cycles < 20000000u && !found; cycles++) {
    if (queued < encoder.pos && !pio_emu_tx_full(&emu)) {
      pio_emu_tx_put(&emu, pulses[queued++]);
    }
    pio_emu_step(&emu);
    bool current_pin = emu.set_pins;
    if (previous_pin && !current_pin) {
      if (have_edge) {
        uint64_t interval = emu.cycle_count - previous_edge;
        ASSERT(interval <= UINT16_MAX);
        if (mfm_feed(&decoder, (uint16_t)interval, &sector)) found = true;
      }
      previous_edge = emu.cycle_count;
      have_edge = true;
    }
    previous_pin = current_pin;
  }
  ASSERT(found);
  ASSERT_EQ(sector.cylinder, 7);
  ASSERT_EQ(sector.head, 1);
  ASSERT_EQ(sector.sector, 4);
  ASSERT_MEM_EQ(sector.data, data, sizeof(data));
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
  ASSERT_EQ(words[0], 0xFF17FFA7u);
  ASSERT_EQ(words[1], 0xFDF4FEB4u);
  uint16_t packed[] = {
      words[0] & 0xFFFFu, words[0] >> 16,
      words[1] & 0xFFFFu, words[1] >> 16,
  };
  ASSERT_EQ(packed[0] & 1u, 1);
  ASSERT_EQ(packed[1] & 1u, 1);
  ASSERT_EQ(packed[2] & 1u, 0);
  ASSERT_EQ(packed[3] & 1u, 0);
  ASSERT_EQ(packed[0] >> 1, 0x7FD3u);
  ASSERT_EQ((packed[0] >> 1) - (packed[1] >> 1), 72);
  ASSERT_EQ((packed[1] >> 1) - (packed[2] >> 1), 49);
  ASSERT_EQ((packed[2] >> 1) - (packed[3] >> 1), 96);
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
  RUN_TEST(test_generated_write_mfm_roundtrip);
  RUN_TEST(test_generated_read_program_captures_flux);
  RUN_TEST(test_generated_read_counter_wraps_from_zero);
  RUN_TEST(test_emulator_rejects_invalid_programs_and_handles_32_bit_shifts);
  TEST_RESULTS();
}
