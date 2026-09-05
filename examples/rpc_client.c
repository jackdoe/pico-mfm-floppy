#include "rpc_client.h"
#include <string.h>

static disk_result_t exchange(rpc_client_t *client, uint8_t op,
                              const uint8_t *request, size_t length,
                              uint8_t *response, size_t capacity) {
  if (!client || !client->exchange) {
    return (disk_result_t){.error = DISK_ERR_INVALID};
  }
  disk_result_t result =
      client->exchange(client->ctx, op, request, length, response, capacity);
  if (result.count > capacity || (unsigned)result.error > DISK_ERR_LAST) {
    return (disk_result_t){.error = DISK_ERR_CORRUPT};
  }
  return result;
}

disk_err_t rpc_file_open(rpc_client_t *client, const char *path,
                         f12_open_mode_t mode, uint64_t *handle) {
  if (handle) *handle = 0;
  if (!path || !handle || (mode != F12_OPEN_READ && mode != F12_OPEN_WRITE)) {
    return DISK_ERR_INVALID;
  }
  uint8_t request[16];
  size_t length = 0;
  while (length < sizeof(request) - 1u && path[length] != 0) length++;
  if (length == 0 || length == sizeof(request) - 1u) return DISK_ERR_INVALID;
  request[0] = mode == F12_OPEN_READ ? 'r' : 'w';
  memcpy(request + 1, path, length + 1u);
  uint8_t response[RPC_HANDLE_SIZE];
  disk_result_t result = exchange(client, RPC_OPEN, request, length + 2u,
                                  response, sizeof(response));
  if (result.error != DISK_OK) return result.error;
  if (result.count != sizeof(response)) return DISK_ERR_CORRUPT;
  uint64_t id = load_le64(response);
  if (id == 0) return DISK_ERR_CORRUPT;
  *handle = id;
  return DISK_OK;
}

static disk_err_t finish(rpc_client_t *client, uint8_t op, uint64_t handle) {
  uint8_t request[RPC_HANDLE_SIZE];
  store_le64(request, handle);
  return exchange(client, op, request, sizeof(request), NULL, 0).error;
}

disk_err_t rpc_file_close(rpc_client_t *client, uint64_t handle) {
  return finish(client, RPC_CLOSE, handle);
}

disk_err_t rpc_file_abort(rpc_client_t *client, uint64_t handle) {
  return finish(client, RPC_ABORT, handle);
}

disk_result_t rpc_file_read(rpc_client_t *client, uint64_t handle, void *buffer,
                            size_t length) {
  if ((!buffer && length != 0) || length > RPC_CHUNK) {
    return (disk_result_t){.error = DISK_ERR_INVALID};
  }
  uint8_t request[RPC_HANDLE_SIZE + 2u];
  store_le64(request, handle);
  store_le16(request + RPC_HANDLE_SIZE, (uint16_t)length);
  return exchange(client, RPC_READ, request, sizeof(request), buffer, length);
}

disk_result_t rpc_file_write(rpc_client_t *client, uint64_t handle,
                             const void *buffer, size_t length) {
  if ((!buffer && length != 0) || length > RPC_CHUNK) {
    return (disk_result_t){.error = DISK_ERR_INVALID};
  }
  uint8_t request[RPC_HANDLE_SIZE + RPC_CHUNK];
  store_le64(request, handle);
  if (length != 0) memcpy(request + RPC_HANDLE_SIZE, buffer, length);
  uint8_t response[2];
  disk_result_t result =
      exchange(client, RPC_WRITE, request, RPC_HANDLE_SIZE + length, response,
               sizeof(response));
  if (result.count == 0 && result.error != DISK_OK) return result;
  if (result.count != sizeof(response)) {
    return (disk_result_t){.error = DISK_ERR_CORRUPT};
  }
  size_t written = load_le16(response);
  if (written > length ||
      (length != 0 && written == 0 && result.error == DISK_OK)) {
    return (disk_result_t){.error = DISK_ERR_CORRUPT};
  }
  return (disk_result_t){result.error, written};
}
