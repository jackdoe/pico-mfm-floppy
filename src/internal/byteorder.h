#ifndef BYTEORDER_H
#define BYTEORDER_H

#include <stdint.h>

static inline uint16_t load_le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static inline uint32_t load_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline void store_le16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

static inline void store_le32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

static inline uint64_t load_le64(const uint8_t *p) {
  return (uint64_t)load_le32(p) | ((uint64_t)load_le32(p + 4) << 32);
}

static inline void store_le64(uint8_t *p, uint64_t value) {
  store_le32(p, (uint32_t)value);
  store_le32(p + 4, (uint32_t)(value >> 32));
}

#endif
