#ifndef RPC_H
#define RPC_H

#include <stdint.h>

#define RPC_PING 1u
#define RPC_MOUNT 2u
#define RPC_UNMOUNT 3u
#define RPC_FORMAT 4u
#define RPC_FSCK 5u
#define RPC_OPEN 6u
#define RPC_CLOSE 7u
#define RPC_READ 8u
#define RPC_WRITE 9u
#define RPC_SEEK 10u
#define RPC_STAT 11u
#define RPC_DELETE 12u
#define RPC_RENAME 13u
#define RPC_LIST 14u
#define RPC_STATUS 15u
#define RPC_MOTOR 16u
#define RPC_SELECT 17u

#define RPC_HDR 4u
#define RPC_CHUNK 512u
#define RPC_MAX_MSG 8192u
#define RPC_STAT_SIZE 19u

static inline uint16_t rpc_load16(const uint8_t *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rpc_load32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline void rpc_store16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

static inline void rpc_store32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

#endif
