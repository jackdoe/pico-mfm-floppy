#include "test.h"
#include "vdisk.h"
#include "rpc_client.h"
#include "rpc_server.h"

static vdisk_t disk;
static f12_t fs;
static uint8_t response[RPC_MAX_MSG];

static disk_result_t loopback(void *ctx, uint8_t op, const uint8_t *request,
                              size_t length, uint8_t *output, size_t capacity) {
  return rpc_dispatch(ctx, op, request, length, output, capacity);
}

static rpc_client_t client = {.exchange = loopback, .ctx = &fs};

static void prepare(void) {
  vdisk_format_valid(&disk);
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
}

static disk_result_t call(uint8_t op, const uint8_t *request, size_t length) {
  memset(response, 0xA5, sizeof(response));
  return rpc_dispatch(&fs, op, request, length, response, sizeof(response));
}

static void write_file(const char *name, const uint8_t *data, size_t length) {
  uint64_t handle;
  ASSERT_EQ(rpc_file_open(&client, name, F12_OPEN_WRITE, &handle), DISK_OK);
  for (size_t offset = 0; offset < length;) {
    size_t count = length - offset;
    if (count > RPC_CHUNK) count = RPC_CHUNK;
    disk_result_t result =
        rpc_file_write(&client, handle, data + offset, count);
    ASSERT_EQ(result.error, DISK_OK);
    ASSERT_EQ(result.count, count);
    offset += result.count;
  }
  ASSERT_EQ(rpc_file_close(&client, handle), DISK_OK);
}

TEST(test_file_roundtrip_and_stale_handles) {
  prepare();
  uint8_t source[RPC_CHUNK];
  for (size_t i = 0; i < sizeof(source); i++) source[i] = (uint8_t)(i * 37u);
  uint64_t writer;
  ASSERT_EQ(rpc_file_open(&client, "ROUND.BIN", F12_OPEN_WRITE, &writer),
            DISK_OK);
  ASSERT_EQ(rpc_file_write(&client, writer, source, sizeof(source)).count,
            sizeof(source));
  ASSERT_EQ(rpc_file_close(&client, writer), DISK_OK);
  uint64_t reader;
  ASSERT_EQ(rpc_file_open(&client, "ROUND.BIN", F12_OPEN_READ, &reader),
            DISK_OK);
  ASSERT(reader != writer);
  ASSERT_EQ(rpc_file_close(&client, writer), DISK_ERR_BAD_HANDLE);
  uint8_t data[RPC_CHUNK];
  disk_result_t result = rpc_file_read(&client, reader, data, sizeof(data));
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, sizeof(data));
  ASSERT_MEM_EQ(source, data, sizeof(data));
  result = rpc_file_read(&client, reader, data, sizeof(data));
  ASSERT_EQ(result.error, DISK_END);
  ASSERT_EQ(result.count, 0);
  ASSERT_EQ(rpc_file_close(&client, reader), DISK_OK);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_partial_read_keeps_bytes_error_and_position) {
  prepare();
  uint8_t source[DISK_SECTOR_SIZE * 4u];
  for (size_t i = 0; i < sizeof(source); i++)
    source[i] = (uint8_t)(i * 13u + 5u);
  write_file("PART.BIN", source, sizeof(source));
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
  ASSERT_EQ(f12_mount(&fs), DISK_OK);
  uint64_t handle;
  ASSERT_EQ(rpc_file_open(&client, "PART.BIN", F12_OPEN_READ, &handle),
            DISK_OK);
  uint8_t seek[RPC_HANDLE_SIZE + 4u];
  store_le64(seek, handle);
  store_le32(seek + RPC_HANDLE_SIZE, DISK_SECTOR_SIZE * 3u - 12u);
  ASSERT_EQ(call(RPC_SEEK, seek, sizeof(seek)).error, DISK_OK);
  disk.fail_track = 2;
  uint8_t data[RPC_CHUNK];
  disk_result_t result = rpc_file_read(&client, handle, data, sizeof(data));
  ASSERT_EQ(result.error, DISK_ERR_TIMEOUT);
  ASSERT_EQ(result.count, 12);
  ASSERT_MEM_EQ(data, source + DISK_SECTOR_SIZE * 3u - 12u, 12);
  disk.fail_track = -1;
  result = rpc_file_read(&client, handle, data, sizeof(data));
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, sizeof(data));
  ASSERT_MEM_EQ(data, source + DISK_SECTOR_SIZE * 3u, sizeof(data));
  ASSERT_EQ(rpc_file_close(&client, handle), DISK_OK);
}

TEST(test_partial_write_can_abort_without_publishing) {
  prepare();
  for (uint16_t cluster = 3; cluster < FAT12_CLUSTER_LIMIT; cluster++) {
    vdisk_set_fat_entry(&disk, cluster, 0x0FFF);
  }
  cache_clear(&fs.cache);
  uint64_t handle;
  ASSERT_EQ(rpc_file_open(&client, "FULL.BIN", F12_OPEN_WRITE, &handle),
            DISK_OK);
  uint8_t data[RPC_CHUNK];
  memset(data, 0x6D, sizeof(data));
  ASSERT_EQ(rpc_file_write(&client, handle, data, 500).count, 500);
  disk_result_t result = rpc_file_write(&client, handle, data, sizeof(data));
  ASSERT_EQ(result.error, DISK_ERR_FULL);
  ASSERT_EQ(result.count, 12);
  ASSERT_EQ(rpc_file_close(&client, handle), DISK_ERR_FULL);
  ASSERT_EQ(rpc_file_abort(&client, handle), DISK_OK);
  f12_stat_t stat;
  ASSERT_EQ(f12_stat(&fs, "FULL.BIN", &stat), DISK_ERR_NOT_FOUND);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT1_START, 2), 0);
  ASSERT_EQ(vdisk_get_fat_copy_entry(&disk, FAT12_FAT2_START, 2), 0);
}

TEST(test_failed_commit_keeps_ownership_and_retries) {
  prepare();
  uint64_t handle;
  ASSERT_EQ(rpc_file_open(&client, "RETRY.BIN", F12_OPEN_WRITE, &handle),
            DISK_OK);
  ASSERT_EQ(rpc_file_write(&client, handle, "retry", 5).count, 5);
  disk.write_status = DISK_ERR_VERIFY;
  ASSERT_EQ(rpc_file_close(&client, handle), DISK_ERR_VERIFY);
  ASSERT_EQ(rpc_file_abort(&client, handle), DISK_ERR_BUSY);
  ASSERT_EQ(call(RPC_UNMOUNT, NULL, 0).error, DISK_ERR_CONFLICT);
  uint64_t another;
  ASSERT_EQ(rpc_file_open(&client, "OTHER.BIN", F12_OPEN_WRITE, &another),
            DISK_ERR_CONFLICT);
  disk.write_status = DISK_OK;
  ASSERT_EQ(rpc_file_close(&client, handle), DISK_OK);
  ASSERT_EQ(rpc_file_close(&client, handle), DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(rpc_file_open(&client, "RETRY.BIN", F12_OPEN_READ, &another),
            DISK_OK);
  uint8_t data[5];
  ASSERT_EQ(rpc_file_read(&client, another, data, sizeof(data)).count,
            sizeof(data));
  ASSERT_MEM_EQ(data, "retry", sizeof(data));
  ASSERT_EQ(rpc_file_close(&client, another), DISK_OK);
}

TEST(test_media_change_drops_remote_ownership) {
  prepare();
  uint64_t stale;
  ASSERT_EQ(rpc_file_open(&client, "OLD.BIN", F12_OPEN_WRITE, &stale), DISK_OK);
  disk.generation++;
  ASSERT_EQ(rpc_file_write(&client, stale, "x", 1).error,
            DISK_ERR_MEDIA_CHANGED);
  ASSERT_EQ(call(RPC_MOUNT, NULL, 0).error, DISK_OK);
  uint64_t current;
  ASSERT_EQ(rpc_file_open(&client, "NEW.BIN", F12_OPEN_WRITE, &current),
            DISK_OK);
  ASSERT(current != stale);
  ASSERT_EQ(rpc_file_abort(&client, stale), DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(rpc_file_abort(&client, current), DISK_OK);
  ASSERT_EQ(f12_init(&fs, vdisk_device(&disk)), DISK_OK);
  ASSERT_EQ(call(RPC_MOUNT, NULL, 0).error, DISK_OK);
  uint64_t fresh;
  ASSERT_EQ(rpc_file_open(&client, "NEW.BIN", F12_OPEN_WRITE, &fresh), DISK_OK);
  ASSERT(fresh != current);
  ASSERT_EQ(rpc_file_close(&client, current), DISK_ERR_BAD_HANDLE);
  ASSERT_EQ(rpc_file_abort(&client, fresh), DISK_OK);
}

TEST(test_request_validation_precedes_mutation) {
  prepare();
  static const struct {
    uint8_t op;
    uint8_t data[20];
    size_t length;
  } invalid[] = {
      {RPC_OPEN, {'x', 'A', 0}, 3},
      {RPC_OPEN, {'w', 'A'}, 2},
      {RPC_OPEN, {'w', 'A', 0, 'B', 0}, 5},
      {RPC_OPEN, {'w', 'A', 0, 0}, 4},
      {RPC_CLOSE, {0}, RPC_HANDLE_SIZE - 1u},
      {RPC_ABORT, {0}, RPC_HANDLE_SIZE + 1u},
      {RPC_READ, {0}, RPC_HANDLE_SIZE + 1u},
      {RPC_WRITE, {0}, RPC_HANDLE_SIZE - 1u},
      {RPC_SEEK, {0}, RPC_HANDLE_SIZE + 3u},
      {RPC_STAT, {'A'}, 1},
      {RPC_DELETE, {'A', 0, 'B', 0}, 4},
      {RPC_RENAME, {'A', 0, 'B'}, 3},
      {RPC_RENAME, {'A', 0, 'B', 0, 'C', 0}, 6},
      {RPC_RENAME, {'A', 'B', 'C', 'D'}, 4},
      {RPC_FSCK, {2}, 1},
      {RPC_FSCK, {0, 0}, 2},
      {RPC_FORMAT, {2, 'A', 0}, 3},
      {RPC_FORMAT, {0, 'A'}, 2},
      {RPC_MOUNT, {0}, 1},
      {RPC_UNMOUNT, {0}, 1},
      {RPC_LIST, {0}, 1},
      {RPC_PING, {0}, 1},
      {255, {0}, 0},
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    disk_result_t result =
        call(invalid[i].op, invalid[i].data, invalid[i].length);
    ASSERT_EQ(result.error, DISK_ERR_INVALID);
    ASSERT_EQ(result.count, 0);
  }
  uint8_t open[] = {'w', 'A', 0};
  ASSERT_EQ(rpc_dispatch(&fs, RPC_OPEN, open, sizeof(open), response,
                         RPC_HANDLE_SIZE - 1u)
                .error,
            DISK_ERR_INVALID);
  ASSERT_EQ(rpc_dispatch(&fs, RPC_OPEN, NULL, sizeof(open), response,
                         sizeof(response))
                .error,
            DISK_ERR_INVALID);
  ASSERT_EQ(
      rpc_dispatch(&fs, RPC_OPEN, open, sizeof(open), NULL, sizeof(response))
          .error,
      DISK_ERR_INVALID);
  ASSERT_EQ(rpc_dispatch(&fs, RPC_PING, open, RPC_MAX_MSG + 1u, response,
                         sizeof(response))
                .error,
            DISK_ERR_INVALID);
  ASSERT_EQ(
      rpc_dispatch(NULL, RPC_PING, NULL, 0, response, sizeof(response)).error,
      DISK_ERR_INVALID);
  ASSERT_EQ(disk.track_writes, 0);
  ASSERT_EQ(call(RPC_UNMOUNT, NULL, 0).error, DISK_OK);
}

TEST(test_response_capacity_does_not_consume_file_data) {
  prepare();
  uint64_t handle;
  ASSERT_EQ(rpc_file_open(&client, "CAP.BIN", F12_OPEN_WRITE, &handle),
            DISK_OK);
  uint8_t write[RPC_HANDLE_SIZE + 1u];
  store_le64(write, handle);
  write[RPC_HANDLE_SIZE] = 'x';
  ASSERT_EQ(
      rpc_dispatch(&fs, RPC_WRITE, write, sizeof(write), response, 1).error,
      DISK_ERR_INVALID);
  ASSERT_EQ(rpc_file_close(&client, handle), DISK_OK);
  f12_stat_t stat;
  ASSERT_EQ(f12_stat(&fs, "CAP.BIN", &stat), DISK_OK);
  ASSERT_EQ(stat.size, 0);
  write_file("CAP.BIN", (const uint8_t *)"data", 4);
  ASSERT_EQ(rpc_file_open(&client, "CAP.BIN", F12_OPEN_READ, &handle), DISK_OK);
  uint8_t read[RPC_HANDLE_SIZE + 2u];
  store_le64(read, handle);
  store_le16(read + RPC_HANDLE_SIZE, 4);
  ASSERT_EQ(rpc_dispatch(&fs, RPC_READ, read, sizeof(read), response, 3).error,
            DISK_ERR_INVALID);
  uint8_t data[4];
  ASSERT_EQ(rpc_file_read(&client, handle, data, sizeof(data)).count,
            sizeof(data));
  ASSERT_MEM_EQ(data, "data", sizeof(data));
  ASSERT_EQ(rpc_file_close(&client, handle), DISK_OK);
}

TEST(test_directory_and_metadata_operations) {
  prepare();
  write_file("ONE.BIN", (const uint8_t *)"one", 3);
  write_file("TWO.BIN", (const uint8_t *)"two", 3);
  disk_result_t result = call(RPC_LIST, NULL, 0);
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, 2u * RPC_STAT_SIZE);
  f12_stat_t stat;
  rpc_decode_stat(response, &stat);
  ASSERT_STR_EQ(stat.name, "ONE.BIN");
  ASSERT_EQ(stat.size, 3);
  ASSERT_EQ(stat.attr, FAT12_ATTR_ARCHIVE);
  rpc_decode_stat(response + RPC_STAT_SIZE, &stat);
  ASSERT_STR_EQ(stat.name, "TWO.BIN");
  result = rpc_dispatch(&fs, RPC_LIST, NULL, 0, response, RPC_STAT_SIZE);
  ASSERT_EQ(result.error, DISK_ERR_FULL);
  ASSERT_EQ(result.count, RPC_STAT_SIZE);
  static const uint8_t rename[] = "ONE.BIN\0THREE.BIN";
  ASSERT_EQ(call(RPC_RENAME, rename, sizeof(rename)).error, DISK_OK);
  static const uint8_t name[] = "THREE.BIN";
  result = call(RPC_STAT, name, sizeof(name));
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, RPC_STAT_SIZE);
  rpc_decode_stat(response, &stat);
  ASSERT_STR_EQ(stat.name, "THREE.BIN");
  ASSERT_EQ(call(RPC_DELETE, name, sizeof(name)).error, DISK_OK);
  ASSERT_EQ(call(RPC_STAT, name, sizeof(name)).error, DISK_ERR_NOT_FOUND);
  ASSERT_EQ(f12_unmount(&fs), DISK_OK);
}

TEST(test_fsck_repairs_and_verifies_convergence) {
  prepare();
  vdisk_set_fat_entry(&disk, 100, 0x0FFF);
  cache_clear(&fs.cache);
  uint8_t repair = 0;
  disk_result_t result = call(RPC_FSCK, &repair, 1);
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, RPC_FSCK_SIZE);
  fat12_fsck_t report;
  rpc_decode_fsck(response, &report);
  ASSERT_EQ(report.lost_clusters, 1);
  ASSERT(!fat12_fsck_clean(&report));
  repair = 1;
  result = call(RPC_FSCK, &repair, 1);
  ASSERT_EQ(result.error, DISK_OK);
  ASSERT_EQ(result.count, RPC_FSCK_SIZE);
  rpc_decode_fsck(response, &report);
  ASSERT_EQ(report.lost_clusters, 1);
  ASSERT_EQ(report.freed, 1);
  ASSERT_EQ(f12_fsck(&fs, &report, false), DISK_OK);
  ASSERT(fat12_fsck_clean(&report));
}

TEST(test_fsck_does_not_report_success_when_verification_fails) {
  prepare();
  vdisk_set_fat_entry(&disk, 100, 0x0FFF);
  cache_clear(&fs.cache);
  uint8_t repair = 1;
  disk.fail_reads_after_write = true;
  disk.read_failure = DISK_ERR_TIMEOUT;
  disk_result_t result = call(RPC_FSCK, &repair, 1);
  ASSERT_EQ(result.error, DISK_ERR_TIMEOUT);
  ASSERT_EQ(result.count, 0);
}

TEST(test_format_and_mount_contract) {
  prepare();
  static const uint8_t format[] = "\0RPCDISK";
  ASSERT_EQ(call(RPC_FORMAT, format, sizeof(format)).error, DISK_OK);
  ASSERT_EQ(call(RPC_MOUNT, NULL, 0).error, DISK_OK);
  ASSERT_EQ(call(RPC_MOUNT, NULL, 0).error, DISK_ERR_ALREADY_MOUNTED);
  fat12_fsck_t report;
  ASSERT_EQ(f12_fsck(&fs, &report, false), DISK_OK);
  ASSERT(fat12_fsck_clean(&report));
}

typedef struct {
  disk_result_t result;
  uint8_t bytes[RPC_HANDLE_SIZE];
} broken_reply_t;

static disk_result_t broken_exchange(void *ctx, uint8_t op,
                                     const uint8_t *request, size_t length,
                                     uint8_t *output, size_t capacity) {
  (void)op;
  (void)request;
  (void)length;
  broken_reply_t *reply = ctx;
  size_t count =
      capacity < sizeof(reply->bytes) ? capacity : sizeof(reply->bytes);
  if (count != 0) memcpy(output, reply->bytes, count);
  return reply->result;
}

TEST(test_client_rejects_invalid_response_counts) {
  broken_reply_t reply = {.result = {DISK_OK, 1}};
  rpc_client_t broken = {.exchange = broken_exchange, .ctx = &reply};
  uint64_t handle;
  ASSERT_EQ(rpc_file_open(&broken, "A", F12_OPEN_READ, &handle),
            DISK_ERR_CORRUPT);
  reply.result.count = RPC_HANDLE_SIZE;
  ASSERT_EQ(rpc_file_open(&broken, "A", F12_OPEN_READ, &handle),
            DISK_ERR_CORRUPT);
  reply.result.count++;
  ASSERT_EQ(rpc_file_open(&broken, "A", F12_OPEN_READ, &handle),
            DISK_ERR_CORRUPT);
  reply.result = (disk_result_t){DISK_OK, 2};
  store_le16(reply.bytes, 2);
  ASSERT_EQ(rpc_file_write(&broken, 1, "x", 1).error, DISK_ERR_CORRUPT);
  store_le16(reply.bytes, 0);
  ASSERT_EQ(rpc_file_write(&broken, 1, "x", 1).error, DISK_ERR_CORRUPT);
  reply.result = (disk_result_t){DISK_ERR_TIMEOUT, 0};
  ASSERT_EQ(rpc_file_write(&broken, 1, "x", 1).error, DISK_ERR_TIMEOUT);
  reply.result = (disk_result_t){DISK_OK, 2};
  uint8_t data;
  ASSERT_EQ(rpc_file_read(&broken, 1, &data, 1).error, DISK_ERR_CORRUPT);
  reply.result = (disk_result_t){(disk_err_t)(DISK_ERR_LAST + 1), 0};
  ASSERT_EQ(rpc_file_close(&broken, 1), DISK_ERR_CORRUPT);
  ASSERT_EQ(rpc_file_open(NULL, "A", F12_OPEN_READ, &handle), DISK_ERR_INVALID);
  ASSERT_EQ(rpc_file_open(&broken, "TOO-LONG-FILENAME", F12_OPEN_READ, &handle),
            DISK_ERR_INVALID);
  ASSERT_EQ(rpc_file_read(&broken, 1, NULL, 1).error, DISK_ERR_INVALID);
  ASSERT_EQ(rpc_file_write(&broken, 1, &data, RPC_CHUNK + 1u).error,
            DISK_ERR_INVALID);
}

int main(void) {
  RUN_TEST(test_file_roundtrip_and_stale_handles);
  RUN_TEST(test_partial_read_keeps_bytes_error_and_position);
  RUN_TEST(test_partial_write_can_abort_without_publishing);
  RUN_TEST(test_failed_commit_keeps_ownership_and_retries);
  RUN_TEST(test_media_change_drops_remote_ownership);
  RUN_TEST(test_request_validation_precedes_mutation);
  RUN_TEST(test_response_capacity_does_not_consume_file_data);
  RUN_TEST(test_directory_and_metadata_operations);
  RUN_TEST(test_fsck_repairs_and_verifies_convergence);
  RUN_TEST(test_fsck_does_not_report_success_when_verification_fails);
  RUN_TEST(test_format_and_mount_contract);
  RUN_TEST(test_client_rejects_invalid_response_counts);
  TEST_RESULTS();
}
