#include "rpc_server.h"
#include <string.h>

static bool request_string(const uint8_t *request, size_t length) {
  return length > 0 && request[length - 1u] == 0 &&
         memchr(request, 0, length - 1u) == NULL;
}

static disk_err_t unmount_if_mounted(f12_t *fs) {
  bool mounted;
  disk_err_t error = f12_is_mounted(fs, &mounted);
  if (error != DISK_OK) return error;
  return mounted ? f12_unmount(fs) : DISK_OK;
}

typedef struct {
  uint8_t *data;
  size_t capacity;
  size_t length;
} rpc_list_t;

static disk_err_t list_entry(void *ctx, const f12_stat_t *stat) {
  rpc_list_t *list = ctx;
  if (list->capacity - list->length < RPC_STAT_SIZE) return DISK_ERR_FULL;
  rpc_encode_stat(list->data + list->length, stat);
  list->length += RPC_STAT_SIZE;
  return DISK_OK;
}

disk_result_t rpc_dispatch(f12_t *fs, uint8_t op, const uint8_t *request,
                           size_t length, uint8_t *response, size_t capacity) {
  disk_result_t result = {.error = DISK_ERR_INVALID};
  if (!fs || (!request && length != 0) || (!response && capacity != 0) ||
      length > RPC_MAX_MSG) {
    return result;
  }
  f12_file_t file = {.token = {.fs = fs}};
  switch (op) {
    case RPC_PING: {
      static const char identity[] = "pico-floppy rpc";
      if (length != 0 || capacity < sizeof(identity) - 1u) break;
      memcpy(response, identity, sizeof(identity) - 1u);
      return (disk_result_t){DISK_OK, sizeof(identity) - 1u};
    }
    case RPC_MOUNT:
      if (length != 0) break;
      result.error = f12_mount(fs);
      break;
    case RPC_UNMOUNT:
      if (length != 0) break;
      result.error = f12_unmount(fs);
      break;
    case RPC_FORMAT:
      if (length < 2 || request[0] > 1 ||
          !request_string(request + 1, length - 1u))
        break;
      result.error = unmount_if_mounted(fs);
      if (result.error == DISK_OK) {
        result.error = f12_format(
            fs, (f12_format_options_t){
                    .label = (const char *)request + 1,
                    .mode = request[0] ? F12_FORMAT_FULL : F12_FORMAT_QUICK,
                });
      }
      break;
    case RPC_FSCK: {
      if (length != 1 || request[0] > 1 || capacity < RPC_FSCK_SIZE) break;
      fat12_fsck_t report;
      result.error = f12_fsck(fs, &report, request[0] != 0);
      if (result.error != DISK_OK) break;
      rpc_encode_fsck(response, &report);
      result.count = RPC_FSCK_SIZE;
      break;
    }
    case RPC_OPEN: {
      if (length < 3 || (request[0] != 'r' && request[0] != 'w') ||
          !request_string(request + 1, length - 1u) ||
          capacity < RPC_HANDLE_SIZE)
        break;
      result.error =
          f12_open(fs, (const char *)request + 1,
                   request[0] == 'w' ? F12_OPEN_WRITE : F12_OPEN_READ, &file);
      if (result.error == DISK_OK) {
        store_le64(response, file.token.id);
        result.count = RPC_HANDLE_SIZE;
      }
      break;
    }
    case RPC_CLOSE:
    case RPC_ABORT:
      if (length != RPC_HANDLE_SIZE) break;
      file.token.id = load_le64(request);
      result.error = op == RPC_CLOSE ? f12_close(&file) : f12_abort(&file);
      break;
    case RPC_READ: {
      if (length != RPC_HANDLE_SIZE + 2u) break;
      uint16_t wanted = load_le16(request + RPC_HANDLE_SIZE);
      if (wanted > RPC_CHUNK || capacity < wanted) break;
      file.token.id = load_le64(request);
      return f12_read(&file, response, wanted);
    }
    case RPC_WRITE: {
      if (length < RPC_HANDLE_SIZE || length > RPC_HANDLE_SIZE + RPC_CHUNK ||
          capacity < 2)
        break;
      file.token.id = load_le64(request);
      disk_result_t written =
          f12_write(&file, request + RPC_HANDLE_SIZE, length - RPC_HANDLE_SIZE);
      store_le16(response, (uint16_t)written.count);
      return (disk_result_t){written.error, 2};
    }
    case RPC_SEEK:
      if (length != RPC_HANDLE_SIZE + 4u) break;
      file.token.id = load_le64(request);
      result.error = f12_seek(&file, load_le32(request + RPC_HANDLE_SIZE));
      break;
    case RPC_STAT: {
      if (!request_string(request, length) || capacity < RPC_STAT_SIZE) break;
      f12_stat_t stat;
      result.error = f12_stat(fs, (const char *)request, &stat);
      if (result.error == DISK_OK) {
        rpc_encode_stat(response, &stat);
        result.count = RPC_STAT_SIZE;
      }
      break;
    }
    case RPC_DELETE:
      if (!request_string(request, length)) break;
      result.error = f12_delete(fs, (const char *)request);
      break;
    case RPC_RENAME: {
      if (length < 4) break;
      const uint8_t *separator = memchr(request, 0, length);
      if (!separator) break;
      size_t from_length = (size_t)(separator - request) + 1u;
      if (!request_string(request + from_length, length - from_length)) break;
      result.error = f12_rename(fs, (const char *)request,
                                (const char *)request + from_length);
      break;
    }
    case RPC_LIST: {
      if (length != 0) break;
      rpc_list_t list = {.data = response, .capacity = capacity};
      result.error = f12_list(fs, list_entry, &list);
      result.count = list.length;
      break;
    }
    default:
      break;
  }
  return result;
}
