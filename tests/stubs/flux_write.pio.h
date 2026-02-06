#ifndef STUB_FLUX_WRITE_PIO_H
#define STUB_FLUX_WRITE_PIO_H

#include "hardware/pio.h"

extern const pio_program_t flux_write_program;

static inline pio_sm_config flux_write_program_get_default_config(uint offset) {
    (void)offset;
    pio_sm_config c = {0};
    return c;
}

void flux_write_program_init(PIO pio, uint sm, uint offset, uint pin);

#endif
