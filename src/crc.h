#ifndef CRC_H
#define CRC_H

#include <stdint.h>
#include <stddef.h>

extern const uint16_t crc16_table[256];

static inline uint16_t crc16_update(uint16_t crc, uint8_t byte) {
    return (crc << 8) ^ crc16_table[((crc >> 8) ^ byte) & 0xFF];
}

static inline uint16_t crc16(const uint8_t *data, size_t len, uint16_t init) {
    uint16_t crc = init;
    for (size_t i = 0; i < len; i++) {
        crc = crc16_update(crc, data[i]);
    }
    return crc;
}

static inline uint16_t crc16_mfm(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    crc = crc16_update(crc, 0xA1);
    crc = crc16_update(crc, 0xA1);
    crc = crc16_update(crc, 0xA1);
    for (size_t i = 0; i < len; i++) {
        crc = crc16_update(crc, data[i]);
    }
    return crc;
}

#endif
