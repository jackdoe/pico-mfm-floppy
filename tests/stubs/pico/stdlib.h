#ifndef STUB_PICO_STDLIB_H
#define STUB_PICO_STDLIB_H

#include <stdint.h>
#include <stdbool.h>

static inline void sleep_ms(uint32_t ms) { (void)ms; }
static inline void sleep_us(uint64_t us) { (void)us; }
static inline void tight_loop_contents(void) {}

#endif
