#include "mfm_probe.h"
#include <string.h>

const char *const mfm_test_pattern_names[MFM_TEST_PATTERNS] = {
    "00", "FF", "55", "AA", "92", "49", "random",
};

unsigned mfm_probe_sector_count(uint32_t sectors) {
  unsigned count = 0;
  while (sectors) {
    sectors &= sectors - 1u;
    count++;
  }
  return count;
}

mfm_probe_counts_t mfm_probe_counts(const mfm_probe_t *probe) {
  mfm_probe_counts_t counts = {.invalid_count = probe->oversized};
  for (unsigned bin = 0; bin < MFM_PROBE_BINS; bin++) {
    uint32_t count = probe->histogram[bin];
    if (bin < MFM_PULSE_FLOOR || bin >= MFM_PULSE_CEILING) {
      counts.invalid_count += count;
    } else if (bin <= mfm_short_limit(&probe->decoder)) {
      counts.short_count += count;
    } else if (bin <= mfm_medium_limit(&probe->decoder)) {
      counts.medium_count += count;
    } else {
      counts.long_count += count;
    }
  }
  counts.total = counts.short_count + counts.medium_count + counts.long_count +
                 counts.invalid_count;
  return counts;
}

bool mfm_probe_clean(const mfm_probe_t *probe) {
  mfm_probe_counts_t counts = mfm_probe_counts(probe);
  return probe->revolutions == MFM_PROBE_REVOLUTIONS &&
         probe->minimum_sectors == DISK_SECTORS_PER_TRACK &&
         probe->decoder.crc_errors == 0 && probe->decoder.format_errors == 0 &&
         probe->wrong_address == 0 && probe->duplicates == 0 &&
         probe->mismatches == 0 && counts.total != 0 &&
         counts.invalid_count <= counts.total / 200u;
}

disk_err_t mfm_probe_track(floppy_t *floppy, uint32_t generation,
                           uint8_t cylinder, uint8_t head,
                           const track_t *expected, mfm_probe_t *probe) {
  if (!probe) return DISK_ERR_INVALID;
  memset(probe, 0, sizeof(*probe));
  mfm_init(&probe->decoder);
  if (!disk_ch_valid(cylinder, head) ||
      (expected && (expected->cylinder != cylinder || expected->head != head ||
                    expected->valid != DISK_TRACK_VALID))) {
    return DISK_ERR_INVALID;
  }
  disk_err_t error = floppy_seek(floppy, cylinder);
  if (error == DISK_OK) error = floppy_side_select(floppy, head);
  if (error == DISK_OK) error = floppy_flux_begin(floppy, generation);
  if (error != DISK_OK) return error;

  floppy_pulse_t pulses[64];
  mfm_sector_t sector;
  bool primed = false;
  bool previous_index = false;
  bool started = false;
  uint32_t seen = 0;
  probe->minimum_sectors = DISK_SECTORS_PER_TRACK;
  while (error == DISK_OK && probe->revolutions < MFM_PROBE_REVOLUTIONS) {
    disk_result_t result =
        floppy_flux_read(floppy, pulses, sizeof(pulses) / sizeof(pulses[0]));
    error = result.error;
    for (size_t i = 0; i < result.count; i++) {
      const floppy_pulse_t *pulse = &pulses[i];
      bool boundary = primed && previous_index && !pulse->index;
      previous_index = pulse->index;
      primed = true;
      if (boundary) {
        if (started) {
          unsigned sectors = mfm_probe_sector_count(seen);
          if (sectors < probe->minimum_sectors)
            probe->minimum_sectors = sectors;
          probe->revolutions++;
          if (probe->revolutions == MFM_PROBE_REVOLUTIONS) break;
        }
        started = true;
        seen = 0;
        mfm_reset(&probe->decoder);
      }
      if (!started) continue;
      if (pulse->delta < MFM_PROBE_BINS)
        probe->histogram[pulse->delta]++;
      else
        probe->oversized++;
      if (!mfm_feed(&probe->decoder, pulse->delta, &sector)) continue;
      if (sector.cylinder != cylinder || sector.head != head) {
        probe->wrong_address++;
        continue;
      }
      uint32_t bit = 1u << sector.sector;
      if ((seen & bit) != 0) probe->duplicates++;
      seen |= bit;
      probe->sectors |= bit;
      if (expected && memcmp(sector.data, expected->data[sector.sector],
                             DISK_SECTOR_SIZE) != 0) {
        probe->mismatches++;
      }
    }
  }
  disk_err_t cleanup = floppy_flux_end(floppy);
  return error != DISK_OK ? error : cleanup;
}

bool mfm_test_fill(track_t *track, unsigned pattern, uint32_t round) {
  static const uint8_t values[] = {0x00, 0xFF, 0x55, 0xAA, 0x92, 0x49};
  if (!track || !disk_ch_valid(track->cylinder, track->head) ||
      pattern >= MFM_TEST_PATTERNS)
    return false;
  uint32_t state = 0x9E3779B9u ^ (round * 0x85EBCA6Bu) ^
                   ((uint32_t)track->cylinder << 8u) ^ track->head;
  for (unsigned sector = 0; sector < DISK_SECTORS_PER_TRACK; sector++) {
    for (unsigned byte = 0; byte < DISK_SECTOR_SIZE; byte++) {
      if (pattern < sizeof(values)) {
        track->data[sector][byte] = values[pattern];
      } else {
        state = state * 1664525u + 1013904223u;
        track->data[sector][byte] = (uint8_t)(state >> 24u);
      }
    }
  }
  track->valid = DISK_TRACK_VALID;
  return true;
}
