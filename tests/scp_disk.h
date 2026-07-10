#ifndef SCP_DISK_H
#define SCP_DISK_H

#include "flux_sim.h"
#include "../src/mfm_decode.h"
#include <string.h>

typedef struct {
    flux_sim_t sim;
    bool initialized;
} scp_disk_t;

static block_status_t scp_disk_read_track(void *ctx, uint32_t expected_generation,
                                          uint8_t cylinder, uint8_t head,
                                          track_t *track) {
    scp_disk_t *disk = ctx;
    if (!disk || !disk->initialized || !track ||
        !disk_ch_valid(cylinder, head)) return BLOCK_ERR_INVALID;
    if (expected_generation != 1u) return BLOCK_ERR_MEDIA_CHANGED;

    memset(track, 0, sizeof(*track));
    track->cylinder = cylinder;
    track->head = head;

    bool sought = false;
    for (uint8_t rev = 0;
         rev < disk->sim.num_revolutions && track->valid != DISK_TRACK_VALID;
         rev++) {
        if (!flux_sim_seek(&disk->sim, cylinder, head, rev)) continue;
        sought = true;

        mfm_t mfm;
        mfm_init(&mfm);
        mfm_sector_t sector;
        uint16_t delta;

        while (flux_sim_next(&disk->sim, &delta)) {
            if (!mfm_feed(&mfm, delta, &sector)) continue;
            if (sector.cylinder != cylinder || sector.head != head ||
                sector.sector >= DISK_SECTORS_PER_TRACK) continue;
            uint8_t index = sector.sector;
            if (track_has(track, index)) continue;
            memcpy(track->data[index], sector.data, DISK_SECTOR_SIZE);
            track_mark(track, index);
        }
    }

    if (!sought) return BLOCK_ERR_CORRUPT;
    return track->valid == DISK_TRACK_VALID ? BLOCK_OK : BLOCK_ERR_CRC;
}

static block_status_t scp_disk_media_generation(void *ctx,
                                                uint32_t *generation) {
    scp_disk_t *disk = ctx;
    if (!disk || !disk->initialized || !generation) return BLOCK_ERR_INVALID;
    *generation = 1;
    return BLOCK_OK;
}

static block_status_t scp_disk_write_protected(void *ctx,
                                               bool *write_protected) {
    scp_disk_t *disk = ctx;
    if (!disk || !disk->initialized || !write_protected) {
        return BLOCK_ERR_INVALID;
    }
    *write_protected = true;
    return BLOCK_OK;
}

static bool scp_disk_init(scp_disk_t *disk, uint8_t *data, size_t size) {
    if (!disk || !data) return false;
    memset(disk, 0, sizeof(*disk));
    if (!flux_sim_open_scp(&disk->sim, data, size)) return false;
    disk->initialized = true;
    return true;
}

static void scp_disk_deinit(scp_disk_t *disk) {
    if (!disk || !disk->initialized) return;
    flux_sim_close(&disk->sim);
    disk->initialized = false;
}

static inline block_device_t scp_disk_device(scp_disk_t *disk) {
    return (block_device_t){
        .read_track = scp_disk_read_track,
        .write_track = NULL,
        .media_generation = scp_disk_media_generation,
        .write_protected = scp_disk_write_protected,
        .ctx = disk,
    };
}

#endif
