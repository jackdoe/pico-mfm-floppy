#ifndef RPC_SERVER_H
#define RPC_SERVER_H

#include "f12.h"
#include "rpc.h"

disk_result_t rpc_dispatch(f12_t *fs, uint8_t op, const uint8_t *request,
                           size_t length, uint8_t *response, size_t capacity);

#endif
