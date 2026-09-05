#include "rpc.h"
#include <string.h>

void rpc_encode_stat(uint8_t out[RPC_STAT_SIZE], const f12_stat_t *stat) {
  store_le32(out, stat->size);
  out[4] = stat->attr;
  memcpy(out + 5, stat->name, sizeof(stat->name));
}

void rpc_decode_stat(const uint8_t in[RPC_STAT_SIZE], f12_stat_t *stat) {
  *stat = (f12_stat_t){.size = load_le32(in), .attr = in[4]};
  memcpy(stat->name, in + 5, sizeof(stat->name));
  stat->name[sizeof(stat->name) - 1u] = 0;
}

void rpc_encode_fsck(uint8_t out[RPC_FSCK_SIZE], const fat12_fsck_t *report) {
  store_le16(out + 0, report->files);
  store_le16(out + 2, report->directories);
  store_le16(out + 4, report->lost_clusters);
  store_le16(out + 6, report->crosslinked);
  store_le16(out + 8, report->loops);
  store_le16(out + 10, report->broken_chains);
  store_le16(out + 12, report->size_mismatches);
  store_le16(out + 14, report->truncated_files);
  store_le16(out + 16, report->removed_directories);
  store_le16(out + 18, report->duplicate_names);
  store_le16(out + 20, report->removed_duplicates);
  store_le16(out + 22, report->freed_tails);
  store_le16(out + 24, report->freed);
  store_le32(out + 26, report->fat1_score);
  store_le32(out + 30, report->fat2_score);
  out[34] = report->authoritative_fat;
  out[35] = (uint8_t)((report->fat_mismatch ? 1u : 0u) |
                      (report->fat_markers_invalid ? 2u : 0u) |
                      (report->fat_ambiguous ? 4u : 0u) |
                      (report->repaired_fat1 ? 8u : 0u) |
                      (report->repaired_fat2 ? 16u : 0u));
}

void rpc_decode_fsck(const uint8_t in[RPC_FSCK_SIZE], fat12_fsck_t *report) {
  *report = (fat12_fsck_t){
      .files = load_le16(in + 0),
      .directories = load_le16(in + 2),
      .lost_clusters = load_le16(in + 4),
      .crosslinked = load_le16(in + 6),
      .loops = load_le16(in + 8),
      .broken_chains = load_le16(in + 10),
      .size_mismatches = load_le16(in + 12),
      .truncated_files = load_le16(in + 14),
      .removed_directories = load_le16(in + 16),
      .duplicate_names = load_le16(in + 18),
      .removed_duplicates = load_le16(in + 20),
      .freed_tails = load_le16(in + 22),
      .freed = load_le16(in + 24),
      .fat1_score = load_le32(in + 26),
      .fat2_score = load_le32(in + 30),
      .authoritative_fat = in[34],
      .fat_mismatch = (in[35] & 1u) != 0,
      .fat_markers_invalid = (in[35] & 2u) != 0,
      .fat_ambiguous = (in[35] & 4u) != 0,
      .repaired_fat1 = (in[35] & 8u) != 0,
      .repaired_fat2 = (in[35] & 16u) != 0,
  };
}
