#ifndef STUB_HARDWARE_CLOCKS_H
#define STUB_HARDWARE_CLOCKS_H

#include <stdint.h>

typedef enum { clk_sys = 0 } clock_handle_t;

static inline uint32_t clock_get_hz(clock_handle_t clk) {
    (void)clk;
    return 150000000;
}

#endif
