#ifndef STUB_HARDWARE_DMA_H
#define STUB_HARDWARE_DMA_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/pio.h"

enum dma_channel_transfer_size { DMA_SIZE_8 = 0, DMA_SIZE_16 = 1, DMA_SIZE_32 = 2 };

typedef struct {
  enum dma_channel_transfer_size size;
  bool read_increment;
  bool write_increment;
  bool ring_write;
  bool high_priority;
  uint ring_bits;
  uint dreq;
} dma_channel_config;

typedef struct {
  volatile uint32_t transfer_count;
} dma_channel_hw_t;

static inline dma_channel_config dma_channel_get_default_config(uint channel) {
  (void)channel;
  dma_channel_config c = {0};
  return c;
}

static inline void channel_config_set_transfer_data_size(dma_channel_config *c,
                                                         enum dma_channel_transfer_size size) {
  c->size = size;
}

static inline void channel_config_set_read_increment(dma_channel_config *c, bool incr) {
  c->read_increment = incr;
}

static inline void channel_config_set_write_increment(dma_channel_config *c, bool incr) {
  c->write_increment = incr;
}

static inline void channel_config_set_ring(dma_channel_config *c, bool write, uint size_bits) {
  c->ring_write = write;
  c->ring_bits = size_bits;
}

static inline void channel_config_set_dreq(dma_channel_config *c, uint dreq) {
  c->dreq = dreq;
}

static inline void channel_config_set_high_priority(dma_channel_config *c,
                                                    bool high_priority) {
  c->high_priority = high_priority;
}

int dma_claim_unused_channel(bool required);
void dma_channel_unclaim(uint channel);
void dma_channel_configure(uint channel, const dma_channel_config *config,
                           volatile void *write_addr, const volatile void *read_addr,
                           uint transfer_count, bool trigger);
bool dma_channel_is_busy(uint channel);
void dma_channel_abort(uint channel);
dma_channel_hw_t *dma_channel_hw_addr(uint channel);

#endif
