#ifndef PIO_EMU_H
#define PIO_EMU_H

#include <stdint.h>
#include <stdbool.h>

#define PIO_EMU_FIFO_DEPTH 8
#define PIO_EMU_MAX_PROGRAM 32

enum pio_emu_op {
    PIO_OP_JMP  = 0,
    PIO_OP_WAIT = 1,
    PIO_OP_IN   = 2,
    PIO_OP_OUT  = 3,
    PIO_OP_PUSH_PULL = 4,
    PIO_OP_MOV  = 5,
    PIO_OP_IRQ  = 6,
    PIO_OP_SET  = 7,
};

enum pio_emu_jmp_cond {
    JMP_ALWAYS = 0,
    JMP_NOT_X  = 1,
    JMP_X_DEC  = 2,
    JMP_NOT_Y  = 3,
    JMP_Y_DEC  = 4,
    JMP_X_NE_Y = 5,
    JMP_PIN    = 6,
    JMP_NOT_OSRE = 7,
};

enum pio_emu_in_src {
    IN_PINS = 0,
    IN_X    = 1,
    IN_Y    = 2,
    IN_NULL = 3,
    IN_ISR  = 6,
    IN_OSR  = 7,
};

enum pio_emu_out_dst {
    OUT_PINS    = 0,
    OUT_X       = 1,
    OUT_Y       = 2,
    OUT_NULL    = 3,
    OUT_PINDIRS = 4,
    OUT_PC      = 5,
    OUT_ISR     = 6,
    OUT_EXEC    = 7,
};

enum pio_emu_set_dst {
    SET_PINS    = 0,
    SET_X       = 1,
    SET_Y       = 2,
    SET_PINDIRS = 4,
};

typedef struct {
    uint16_t program[PIO_EMU_MAX_PROGRAM];
    uint8_t program_len;
    uint8_t wrap_target;
    uint8_t wrap;

    uint32_t x;
    uint32_t y;
    uint32_t isr;
    uint32_t osr;
    uint8_t isr_shift_count;
    uint8_t osr_shift_count;
    uint8_t pc;

    bool in_shift_right;
    bool out_shift_right;
    uint8_t autopush_threshold;
    uint8_t autopull_threshold;

    uint32_t rx_fifo[PIO_EMU_FIFO_DEPTH];
    uint8_t rx_head;
    uint8_t rx_count;

    uint32_t tx_fifo[PIO_EMU_FIFO_DEPTH];
    uint8_t tx_head;
    uint8_t tx_count;

    uint32_t set_pins;
    uint32_t pin_values;
    bool jmp_pin;

    uint8_t delay_remaining;
    uint64_t cycle_count;
    bool stalled;
} pio_emu_t;

void pio_emu_init(pio_emu_t *emu);
void pio_emu_load(pio_emu_t *emu, const uint16_t *program, uint8_t len,
                  uint8_t wrap_target, uint8_t wrap);
void pio_emu_step(pio_emu_t *emu);
void pio_emu_run(pio_emu_t *emu, uint32_t cycles);

bool pio_emu_rx_empty(pio_emu_t *emu);
uint32_t pio_emu_rx_get(pio_emu_t *emu);
bool pio_emu_tx_full(pio_emu_t *emu);
void pio_emu_tx_put(pio_emu_t *emu, uint32_t data);

#endif
