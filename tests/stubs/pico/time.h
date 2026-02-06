#ifndef STUB_PICO_TIME_H
#define STUB_PICO_TIME_H

#include <stdint.h>
#include <stdbool.h>

typedef uint64_t absolute_time_t;

struct repeating_timer {
    void *user_data;
};

typedef bool (*repeating_timer_callback_t)(struct repeating_timer *t);

static inline absolute_time_t get_absolute_time(void) { return 0; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { (void)t; return 0; }

static inline bool add_repeating_timer_ms(int32_t delay_ms, repeating_timer_callback_t cb,
                                           void *user_data, struct repeating_timer *out) {
    (void)delay_ms; (void)cb;
    out->user_data = user_data;
    return true;
}

static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t status) { (void)status; }

#endif
