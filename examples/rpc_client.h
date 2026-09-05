#ifndef RPC_CLIENT_H
#define RPC_CLIENT_H

#include "rpc.h"

typedef disk_result_t (*rpc_exchange_t)(void *ctx, uint8_t op,
                                        const uint8_t *request, size_t length,
                                        uint8_t *response, size_t capacity);

typedef struct {
  rpc_exchange_t exchange;
  void *ctx;
} rpc_client_t;

disk_err_t rpc_file_open(rpc_client_t *client, const char *path,
                         f12_open_mode_t mode, uint64_t *handle);
disk_err_t rpc_file_close(rpc_client_t *client, uint64_t handle);
disk_err_t rpc_file_abort(rpc_client_t *client, uint64_t handle);
disk_result_t rpc_file_read(rpc_client_t *client, uint64_t handle, void *buffer,
                            size_t length);
disk_result_t rpc_file_write(rpc_client_t *client, uint64_t handle,
                             const void *buffer, size_t length);

#endif
