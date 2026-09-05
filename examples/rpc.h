#ifndef RPC_H
#define RPC_H

#include "f12.h"
#include "internal/byteorder.h"

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
#define RPC_ABORT 18u

#define RPC_HDR 4u
#define RPC_CHUNK 512u
#define RPC_MAX_MSG 8192u
#define RPC_HANDLE_SIZE 8u
#define RPC_STAT_SIZE 18u
#define RPC_FSCK_SIZE 36u

void rpc_encode_stat(uint8_t out[RPC_STAT_SIZE], const f12_stat_t *stat);
void rpc_decode_stat(const uint8_t in[RPC_STAT_SIZE], f12_stat_t *stat);
void rpc_encode_fsck(uint8_t out[RPC_FSCK_SIZE], const fat12_fsck_t *report);
void rpc_decode_fsck(const uint8_t in[RPC_FSCK_SIZE], fat12_fsck_t *report);

#endif
