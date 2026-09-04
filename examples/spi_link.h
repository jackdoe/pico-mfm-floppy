#ifndef SPI_LINK_H
#define SPI_LINK_H

#include <stdint.h>
#include "hardware/spi.h"

#define SPI_LINK_HZ 1000000u
#define SPI_LINK_MAGIC 0xA5u
#define SPI_LINK_PAYLOAD 128u

#define SPI_LINK_OFF_MAGIC 0u
#define SPI_LINK_OFF_SEQ 1u
#define SPI_LINK_OFF_ACK 2u
#define SPI_LINK_OFF_LEN 3u
#define SPI_LINK_OFF_DATA 4u
#define SPI_LINK_OFF_XSUM (SPI_LINK_OFF_DATA + SPI_LINK_PAYLOAD)
#define SPI_LINK_FRAME (SPI_LINK_OFF_XSUM + 1u)
#define SPI_LINK_WIRE (SPI_LINK_FRAME + 1u)

#define SPI_LINK_MASTER_SPI spi1
#define SPI_LINK_MASTER_CS 22u
#define SPI_LINK_MASTER_SCK 26u
#define SPI_LINK_MASTER_TX 27u
#define SPI_LINK_MASTER_RX 28u

#define SPI_LINK_SLAVE_SPI spi0
#define SPI_LINK_SLAVE_RX 0u
#define SPI_LINK_SLAVE_CSN 1u
#define SPI_LINK_SLAVE_SCK 2u
#define SPI_LINK_SLAVE_TX 3u
#define SPI_LINK_SLAVE_CS 4u

typedef struct {
  uint8_t pending[SPI_LINK_PAYLOAD];
  uint8_t pending_len;
  uint8_t tx_seq;
  uint8_t rx_seen;
  uint8_t queue[2048];
  uint32_t queue_read;
  uint32_t queue_write;
} spi_link_t;

static inline uint8_t spi_link_xsum(const uint8_t *frame) {
  uint8_t sum = 0;
  for (uint32_t i = 0; i < SPI_LINK_OFF_XSUM; i++) sum ^= frame[i];
  return sum;
}

static inline bool spi_link_frame_ok(const uint8_t *frame) {
  return frame[SPI_LINK_OFF_MAGIC] == SPI_LINK_MAGIC &&
         frame[SPI_LINK_OFF_LEN] <= SPI_LINK_PAYLOAD &&
         frame[SPI_LINK_OFF_XSUM] == spi_link_xsum(frame);
}

static inline void spi_link_build(const spi_link_t *link, uint8_t *frame) {
  memset(frame, 0, SPI_LINK_FRAME);
  frame[SPI_LINK_OFF_MAGIC] = SPI_LINK_MAGIC;
  frame[SPI_LINK_OFF_SEQ] = link->tx_seq;
  frame[SPI_LINK_OFF_ACK] = link->rx_seen;
  frame[SPI_LINK_OFF_LEN] = link->pending_len;
  memcpy(&frame[SPI_LINK_OFF_DATA], link->pending, link->pending_len);
  frame[SPI_LINK_OFF_XSUM] = spi_link_xsum(frame);
}

static inline void spi_link_accept(spi_link_t *link, const uint8_t *frame) {
  if (frame[SPI_LINK_OFF_ACK] == link->tx_seq) link->pending_len = 0;
  if (frame[SPI_LINK_OFF_SEQ] == link->rx_seen) return;
  link->rx_seen = frame[SPI_LINK_OFF_SEQ];
  for (uint8_t i = 0; i < frame[SPI_LINK_OFF_LEN]; i++) {
    if (link->queue_write - link->queue_read < sizeof(link->queue)) {
      link->queue[link->queue_write++ % sizeof(link->queue)] =
          frame[SPI_LINK_OFF_DATA + i];
    }
  }
}

static inline uint32_t spi_link_drain(spi_link_t *link, uint8_t *out,
                                      uint32_t want) {
  uint32_t got = 0;
  while (got < want && link->queue_read != link->queue_write) {
    out[got++] = link->queue[link->queue_read++ % sizeof(link->queue)];
  }
  return got;
}

static inline uint8_t spi_link_stage(spi_link_t *link, const uint8_t *data,
                                     uint32_t remaining) {
  uint8_t chunk = remaining > SPI_LINK_PAYLOAD ? (uint8_t)SPI_LINK_PAYLOAD
                                               : (uint8_t)remaining;
  memcpy(link->pending, data, chunk);
  link->pending_len = chunk;
  link->tx_seq++;
  return chunk;
}

#endif
