#ifndef STUB_PICO_STDLIB_H
#define STUB_PICO_STDLIB_H

#include <stdint.h>
#include <stdbool.h>

extern uint64_t pico_test_time_us;

static inline void sleep_ms(uint32_t ms) { pico_test_time_us += (uint64_t)ms * 1000u; }
static inline void sleep_us(uint64_t us) { pico_test_time_us += us; }
static inline void tight_loop_contents(void) { pico_test_time_us++; }

#endif
