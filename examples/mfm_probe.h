#ifndef MFM_PROBE_H
#define MFM_PROBE_H

#include "floppy.h"
#include "mfm.h"

#define MFM_PROBE_BINS 128u
#define MFM_PROBE_REVOLUTIONS 3u
#define MFM_TEST_PATTERNS 7u

typedef struct {
  mfm_t decoder;
  uint32_t histogram[MFM_PROBE_BINS];
  uint32_t oversized;
  uint32_t sectors;
  uint32_t revolutions;
  uint32_t minimum_sectors;
  uint32_t wrong_address;
  uint32_t duplicates;
  uint32_t mismatches;
} mfm_probe_t;

typedef struct {
  uint32_t short_count;
  uint32_t medium_count;
  uint32_t long_count;
  uint32_t invalid_count;
  uint32_t total;
} mfm_probe_counts_t;

extern const char *const mfm_test_pattern_names[MFM_TEST_PATTERNS];

unsigned mfm_probe_sector_count(uint32_t sectors);
mfm_probe_counts_t mfm_probe_counts(const mfm_probe_t *probe);
bool mfm_probe_clean(const mfm_probe_t *probe);
disk_err_t mfm_probe_track(floppy_t *floppy, uint32_t generation,
                           uint8_t cylinder, uint8_t head,
                           const track_t *expected, mfm_probe_t *probe);
bool mfm_test_fill(track_t *track, unsigned pattern, uint32_t round);

#endif
