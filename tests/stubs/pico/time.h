#ifndef STUB_PICO_TIME_H
#define STUB_PICO_TIME_H

#include <stdint.h>
#include <stdbool.h>

typedef uint64_t absolute_time_t;

extern uint64_t pico_test_time_us;

static inline absolute_time_t get_absolute_time(void) {
    return pico_test_time_us;
}

static inline uint32_t to_ms_since_boot(absolute_time_t t) {
    return (uint32_t)(t / 1000u);
}

#endif
