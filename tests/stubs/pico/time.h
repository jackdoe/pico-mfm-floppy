#ifndef STUB_PICO_TIME_H
#define STUB_PICO_TIME_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

typedef uint64_t absolute_time_t;

struct repeating_timer {
    void *user_data;
};

typedef bool (*repeating_timer_callback_t)(struct repeating_timer *t);

static inline absolute_time_t get_absolute_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000u);
}

static inline uint32_t to_ms_since_boot(absolute_time_t t) {
    return (uint32_t)(t / 1000u);
}

static inline bool add_repeating_timer_ms(int32_t delay_ms, repeating_timer_callback_t cb,
                                           void *user_data, struct repeating_timer *out) {
    (void)delay_ms; (void)cb;
    out->user_data = user_data;
    return true;
}

#endif
