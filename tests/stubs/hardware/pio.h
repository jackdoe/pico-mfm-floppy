#ifndef STUB_HARDWARE_PIO_H
#define STUB_HARDWARE_PIO_H

#include <stdint.h>
#include <stdbool.h>

typedef unsigned int uint;
typedef struct { int id; } pio_hw_t;
typedef pio_hw_t *PIO;

typedef struct { int dummy; } pio_program_t;

enum pio_fifo_join { PIO_FIFO_JOIN_RX = 0, PIO_FIFO_JOIN_TX = 1 };
typedef struct { int dummy; } pio_sm_config;

static inline pio_sm_config pio_get_default_config(uint offset) { (void)offset; pio_sm_config c = {0}; return c; }
static inline void sm_config_set_jmp_pin(pio_sm_config *c, uint pin) { (void)c; (void)pin; }
static inline void sm_config_set_in_pins(pio_sm_config *c, uint pin) { (void)c; (void)pin; }
static inline void sm_config_set_set_pins(pio_sm_config *c, uint pin, uint count) { (void)c; (void)pin; (void)count; }
static inline void sm_config_set_out_shift(pio_sm_config *c, bool right, bool autopull, uint bits) { (void)c; (void)right; (void)autopull; (void)bits; }
static inline void sm_config_set_in_shift(pio_sm_config *c, bool right, bool autopush, uint bits) { (void)c; (void)right; (void)autopush; (void)bits; }
static inline void sm_config_set_fifo_join(pio_sm_config *c, enum pio_fifo_join join) { (void)c; (void)join; }
static inline void sm_config_set_clkdiv(pio_sm_config *c, float div) { (void)c; (void)div; }

uint pio_add_program(PIO pio, const pio_program_t *program);
uint pio_claim_unused_sm(PIO pio, bool required);
void pio_sm_init(PIO pio, uint sm, uint offset, const pio_sm_config *config);
void pio_sm_exec(PIO pio, uint sm, uint instr);
void pio_sm_restart(PIO pio, uint sm);
void pio_sm_clear_fifos(PIO pio, uint sm);
void pio_sm_set_enabled(PIO pio, uint sm, bool enabled);
void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint32_t values, uint32_t mask);
void pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin, uint count, bool is_out);
void pio_gpio_init(PIO pio, uint pin);
bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm);
bool pio_sm_is_tx_fifo_empty(PIO pio, uint sm);
uint32_t pio_sm_get_blocking(PIO pio, uint sm);
void pio_sm_put_blocking(PIO pio, uint sm, uint32_t data);

extern PIO pio0;
extern PIO pio1;

static inline uint pio_encode_jmp(uint addr) { return addr; }
static inline uint pio_encode_set(uint dest, uint value) { (void)dest; return value; }
enum { pio_x = 0 };

#endif
