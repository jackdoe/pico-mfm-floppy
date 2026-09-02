#ifndef SCP_FIXTURE_H
#define SCP_FIXTURE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static inline uint8_t *scp_fixture_load(const char *path, size_t *size) {
  if (!path || !size) return NULL;
  *size = 0;
  FILE *file = fopen(path, "rb");
  if (!file) return NULL;
  uint8_t *data = NULL;
  long end = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1L;
  if (end > 0 && fseek(file, 0, SEEK_SET) == 0) {
    data = malloc((size_t)end);
    if (data && fread(data, 1, (size_t)end, file) != (size_t)end) {
      free(data);
      data = NULL;
    }
  }
  if (fclose(file) != 0 && data) {
    free(data);
    data = NULL;
  }
  if (data) *size = (size_t)end;
  return data;
}

#endif
