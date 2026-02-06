#ifndef SCP_DISK_H
#define SCP_DISK_H

#include "flux_sim.h"
#include "../src/floppy.h"
#include "../src/mfm_decode.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *scp_data;
    size_t scp_size;
    uint8_t resolution;
    uint8_t num_revolutions;
} scp_disk_t;

static bool scp_disk_read(void *ctx, sector_t *sector) {
    scp_disk_t *disk = (scp_disk_t *)ctx;

    flux_sim_t sim;
    if (!flux_sim_open_scp(&sim, disk->scp_data, disk->scp_size))
        return false;

    sector->valid = false;

    for (int rev = 0; rev < disk->num_revolutions; rev++) {
        if (!flux_sim_seek(&sim, sector->track, sector->side, rev))
            continue;

        mfm_t mfm;
        mfm_init(&mfm);
        sector_t out;
        uint16_t delta;

        while (flux_sim_next(&sim, &delta)) {
            if (mfm_feed(&mfm, delta, &out)) {
                if (out.valid && out.sector_n == sector->sector_n &&
                    out.track == sector->track && out.side == sector->side) {
                    memcpy(sector->data, out.data, SECTOR_SIZE);
                    sector->valid = true;
                    sector->size_code = out.size_code;
                    flux_sim_close(&sim);
                    return true;
                }
            }
        }
    }

    flux_sim_close(&sim);
    return true;
}

static bool scp_disk_write(void *ctx, track_t *track) {
    (void)ctx; (void)track;
    return false;
}

static bool scp_disk_write_protected(void *ctx) {
    (void)ctx;
    return true;
}

static bool scp_disk_init(scp_disk_t *disk, uint8_t *data, size_t size) {
    memset(disk, 0, sizeof(*disk));
    if (size < 0x10) return false;
    if (data[0] != 'S' || data[1] != 'C' || data[2] != 'P') return false;

    disk->scp_data = data;
    disk->scp_size = size;
    disk->num_revolutions = data[5];
    disk->resolution = data[9];
    return true;
}

#endif
