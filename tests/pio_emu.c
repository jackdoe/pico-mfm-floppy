#include "pio_emu.h"
#include <string.h>

void pio_emu_init(pio_emu_t *emu) {
    if (!emu) return;
    memset(emu, 0, sizeof(*emu));
    emu->in_shift_right = true;
    emu->out_shift_right = true;
}

bool pio_emu_load(pio_emu_t *emu, const uint16_t *program, uint8_t len,
                  uint8_t wrap_target, uint8_t wrap) {
    if (!emu || !program || len == 0 || len > PIO_EMU_MAX_PROGRAM ||
        wrap_target >= len || wrap >= len || wrap_target > wrap) {
        if (emu) emu->fault = true;
        return false;
    }
    memcpy(emu->program, program, len * sizeof(uint16_t));
    emu->program_len = len;
    emu->wrap_target = wrap_target;
    emu->wrap = wrap;
    emu->pc = 0;
    emu->fault = false;
    return true;
}

static bool pio_emu_rx_push(pio_emu_t *emu, uint32_t value) {
    if (emu->rx_count >= PIO_EMU_FIFO_DEPTH) return false;
    uint8_t idx = (uint8_t)((emu->rx_head + emu->rx_count) % PIO_EMU_FIFO_DEPTH);
    emu->rx_fifo[idx] = value;
    emu->rx_count++;
    return true;
}

static bool pio_emu_tx_pop(pio_emu_t *emu, uint32_t *value) {
    if (emu->tx_count == 0) return false;
    *value = emu->tx_fifo[emu->tx_head];
    emu->tx_head = (emu->tx_head + 1) % PIO_EMU_FIFO_DEPTH;
    emu->tx_count--;
    return true;
}

static void pio_emu_do_in(pio_emu_t *emu, uint32_t value, uint8_t bit_count) {
    if (bit_count == 0) bit_count = 32;
    uint32_t mask = (bit_count == 32) ? 0xFFFFFFFF : ((1u << bit_count) - 1);
    value &= mask;

    if (emu->in_shift_right) {
        emu->isr = bit_count == 32 ? value
                                   : (emu->isr >> bit_count) |
                                         (value << (32u - bit_count));
    } else {
        emu->isr = bit_count == 32 ? value : (emu->isr << bit_count) | value;
    }
    emu->isr_shift_count += bit_count;

    if (emu->autopush_threshold > 0 && emu->isr_shift_count >= emu->autopush_threshold) {
        if (!pio_emu_rx_push(emu, emu->isr)) {
            emu->fault = true;
            emu->stalled = true;
            return;
        }
        emu->isr = 0;
        emu->isr_shift_count = 0;
    }
}

static uint32_t pio_emu_do_out(pio_emu_t *emu, uint8_t bit_count) {
    if (bit_count == 0) bit_count = 32;
    uint32_t value;

    if (emu->out_shift_right) {
        value = emu->osr & ((bit_count == 32) ? 0xFFFFFFFF : ((1u << bit_count) - 1));
        emu->osr = bit_count == 32 ? 0 : emu->osr >> bit_count;
    } else {
        value = emu->osr >> (32 - bit_count);
        emu->osr = bit_count == 32 ? 0 : emu->osr << bit_count;
    }
    emu->osr_shift_count += bit_count;

    if (emu->autopull_threshold > 0 && emu->osr_shift_count >= emu->autopull_threshold) {
        uint32_t tx_val;
        if (pio_emu_tx_pop(emu, &tx_val)) {
            emu->osr = tx_val;
            emu->osr_shift_count = 0;
        }
    }

    return value;
}

bool pio_emu_step(pio_emu_t *emu) {
    if (!emu || emu->fault || emu->program_len == 0 || emu->pc >= emu->program_len) {
        if (emu) emu->fault = true;
        return false;
    }
    emu->cycle_count++;

    if (emu->delay_remaining > 0) {
        emu->delay_remaining--;
        return true;
    }

    emu->stalled = false;
    uint16_t instr = emu->program[emu->pc];

    uint8_t op = (instr >> 13) & 0x7;
    uint8_t delay = (instr >> 8) & 0x1F;
    uint8_t arg1 = (instr >> 5) & 0x7;
    uint8_t arg2 = instr & 0x1F;

    bool advance_pc = true;

    switch (op) {
    case PIO_OP_JMP: {
        bool take = false;
        switch (arg1) {
        case JMP_ALWAYS: take = true; break;
        case JMP_NOT_X:  take = (emu->x == 0); break;
        case JMP_X_DEC:  take = (emu->x-- != 0); break;
        case JMP_NOT_Y:  take = (emu->y == 0); break;
        case JMP_Y_DEC:  take = (emu->y-- != 0); break;
        case JMP_X_NE_Y: take = (emu->x != emu->y); break;
        case JMP_PIN:    take = emu->jmp_pin; break;
        case JMP_NOT_OSRE: take = (emu->osr_shift_count < (emu->autopull_threshold ? emu->autopull_threshold : 32)); break;
        }
        if (take) {
            if (arg2 >= emu->program_len) {
                emu->fault = true;
                return false;
            }
            emu->pc = arg2;
            advance_pc = false;
        }
        break;
    }

    case PIO_OP_IN: {
        uint32_t value = 0;
        switch (arg1) {
        case IN_PINS: value = emu->pin_values; break;
        case IN_X:    value = emu->x; break;
        case IN_Y:    value = emu->y; break;
        case IN_NULL: value = 0; break;
        case IN_ISR:  value = emu->isr; break;
        case IN_OSR:  value = emu->osr; break;
        }
        pio_emu_do_in(emu, value, arg2);
        if (emu->fault) return false;
        break;
    }

    case PIO_OP_OUT: {
        uint32_t value = pio_emu_do_out(emu, arg2);
        switch (arg1) {
        case OUT_PINS:    emu->set_pins = value; break;
        case OUT_X:       emu->x = value; break;
        case OUT_Y:       emu->y = value; break;
        case OUT_NULL:    break;
        case OUT_PINDIRS: break;
        case OUT_PC:
            if (value >= emu->program_len) {
                emu->fault = true;
                return false;
            }
            emu->pc = (uint8_t)value;
            advance_pc = false;
            break;
        case OUT_ISR:     emu->isr = value; break;
        case OUT_EXEC:    break;
        }
        break;
    }

    case PIO_OP_PUSH_PULL: {
        bool is_pull = (arg1 >> 2) & 1;
        bool block = arg1 & 1;
        if (is_pull) {
            uint32_t tx_val;
            if (pio_emu_tx_pop(emu, &tx_val)) {
                emu->osr = tx_val;
                emu->osr_shift_count = 0;
            } else if (block) {
                emu->stalled = true;
                advance_pc = false;
                delay = 0;
            }
        } else {
            if (emu->rx_count < PIO_EMU_FIFO_DEPTH) {
                if (!pio_emu_rx_push(emu, emu->isr)) {
                    emu->fault = true;
                    return false;
                }
                emu->isr = 0;
                emu->isr_shift_count = 0;
            } else if (block) {
                emu->stalled = true;
                advance_pc = false;
                delay = 0;
            }
        }
        break;
    }

    case PIO_OP_MOV:
        break;

    case PIO_OP_SET: {
        switch (arg1) {
        case SET_PINS:    emu->set_pins = arg2; break;
        case SET_X:       emu->x = arg2; break;
        case SET_Y:       emu->y = arg2; break;
        case SET_PINDIRS: break;
        }
        break;
    }

    default:
        emu->fault = true;
        return false;
    }

    if (advance_pc) {
        if (emu->pc == emu->wrap) {
            emu->pc = emu->wrap_target;
        } else {
            emu->pc++;
        }
    }

    emu->delay_remaining = delay;
    return true;
}

bool pio_emu_run(pio_emu_t *emu, uint32_t cycles) {
    if (!emu) return false;
    for (uint32_t i = 0; i < cycles && !emu->stalled; i++) {
        if (!pio_emu_step(emu)) return false;
    }
    return !emu->fault;
}

bool pio_emu_rx_empty(pio_emu_t *emu) {
    return !emu || emu->rx_count == 0;
}

uint32_t pio_emu_rx_get(pio_emu_t *emu) {
    if (!emu || emu->rx_count == 0) return 0;
    uint32_t val = emu->rx_fifo[emu->rx_head];
    emu->rx_head = (emu->rx_head + 1) % PIO_EMU_FIFO_DEPTH;
    emu->rx_count--;
    return val;
}

bool pio_emu_tx_full(pio_emu_t *emu) {
    return !emu || emu->tx_count >= PIO_EMU_FIFO_DEPTH;
}

void pio_emu_tx_put(pio_emu_t *emu, uint32_t data) {
    if (!emu || emu->tx_count >= PIO_EMU_FIFO_DEPTH) return;
    uint8_t idx = (uint8_t)((emu->tx_head + emu->tx_count) % PIO_EMU_FIFO_DEPTH);
    emu->tx_fifo[idx] = data;
    emu->tx_count++;
}
