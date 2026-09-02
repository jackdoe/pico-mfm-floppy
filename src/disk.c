#include "disk.h"

const char *disk_strerror(disk_err_t error) {
  switch (error) {
    case DISK_OK: return "success";
    case DISK_END: return "end";
    case DISK_ERR_INVALID: return "invalid argument";
    case DISK_ERR_BUSY: return "busy";
    case DISK_ERR_TIMEOUT: return "timeout";
    case DISK_ERR_CRC: return "CRC error";
    case DISK_ERR_WRONG_TRACK: return "wrong track";
    case DISK_ERR_WRONG_SIDE: return "wrong side";
    case DISK_ERR_NO_TRACK0: return "track zero unavailable";
    case DISK_ERR_MEDIA_CHANGED: return "media changed";
    case DISK_ERR_WRITE_PROTECTED: return "write protected";
    case DISK_ERR_UNDERRUN: return "underrun";
    case DISK_ERR_OVERRUN: return "overrun";
    case DISK_ERR_VERIFY: return "verification failed";
    case DISK_ERR_CORRUPT: return "corrupt";
    case DISK_ERR_IO: return "I/O error";
    case DISK_ERR_NOT_FOUND: return "not found";
    case DISK_ERR_EXISTS: return "already exists";
    case DISK_ERR_FULL: return "disk full";
    case DISK_ERR_READ_ONLY: return "read only";
    case DISK_ERR_AMBIGUOUS: return "ambiguous filesystem state";
    case DISK_ERR_NOT_INITIALIZED: return "not initialized";
    case DISK_ERR_NOT_MOUNTED: return "not mounted";
    case DISK_ERR_ALREADY_MOUNTED: return "already mounted";
    case DISK_ERR_TOO_MANY: return "too many open operations";
    case DISK_ERR_IS_DIR: return "is a directory";
    case DISK_ERR_NOT_DIR: return "not a directory";
    case DISK_ERR_CONFLICT: return "operation conflict";
    case DISK_ERR_BAD_HANDLE: return "bad handle";
  }
  return "unknown error";
}
