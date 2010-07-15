/*
 * Virtio Block Device
 *
 * Copyright IBM, Corp. 2007
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include <qemu-common.h>
#include "virtio-blk.h"

typedef struct VirtIOBlock
{
    VirtIODevice vdev;
    BlockDriverState *bs;
    VirtQueue *vq;
    BlockConf *conf;
    unsigned short sector_mask;
    char sn[BLOCK_SERIAL_STRLEN];
} VirtIOBlock;

static VirtIOBlock *to_virtio_blk(VirtIODevice *vdev)
{
    return (VirtIOBlock *)vdev;
}

static void virtio_blk_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    fprintf(stderr, "virtio_blk_handle_output: should never get here,"
                    "data plane thread should process requests\n");
    exit(1);
}

/* coalesce internal state, copy to pci i/o region 0
 */
static void virtio_blk_update_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOBlock *s = to_virtio_blk(vdev);
    struct virtio_blk_config blkcfg;
    uint64_t capacity;
    int cylinders, heads, secs;

    bdrv_get_geometry(s->bs, &capacity);
    bdrv_get_geometry_hint(s->bs, &cylinders, &heads, &secs);
    memset(&blkcfg, 0, sizeof(blkcfg));
    stq_raw(&blkcfg.capacity, capacity);
    stl_raw(&blkcfg.seg_max, 128 - 2);
    stw_raw(&blkcfg.cylinders, cylinders);
    blkcfg.heads = heads;
    blkcfg.sectors = secs & ~s->sector_mask;
    blkcfg.blk_size = s->conf->logical_block_size;
    blkcfg.size_max = 0;
    blkcfg.physical_block_exp = get_physical_block_exp(s->conf);
    blkcfg.alignment_offset = 0;
    blkcfg.min_io_size = s->conf->min_io_size / blkcfg.blk_size;
    blkcfg.opt_io_size = s->conf->opt_io_size / blkcfg.blk_size;
    memcpy(config, &blkcfg, sizeof(struct virtio_blk_config));
}

static uint32_t virtio_blk_get_features(VirtIODevice *vdev, uint32_t features)
{
    VirtIOBlock *s = to_virtio_blk(vdev);

    features |= (1 << VIRTIO_BLK_F_SEG_MAX);
    features |= (1 << VIRTIO_BLK_F_GEOMETRY);
    features |= (1 << VIRTIO_BLK_F_TOPOLOGY);
    features |= (1 << VIRTIO_BLK_F_BLK_SIZE);

    if (bdrv_enable_write_cache(s->bs))
        features |= (1 << VIRTIO_BLK_F_WCACHE);
    
    if (bdrv_is_read_only(s->bs))
        features |= 1 << VIRTIO_BLK_F_RO;

    return features;
}

VirtIODevice *virtio_blk_init(DeviceState *dev, BlockConf *conf)
{
    VirtIOBlock *s;
    int cylinders, heads, secs;
    DriveInfo *dinfo;

    s = (VirtIOBlock *)virtio_common_init("virtio-blk", VIRTIO_ID_BLOCK,
                                          sizeof(struct virtio_blk_config),
                                          sizeof(VirtIOBlock));

    s->vdev.get_config = virtio_blk_update_config;
    s->vdev.get_features = virtio_blk_get_features;
    s->bs = conf->bs;
    s->conf = conf;
    s->sector_mask = (s->conf->logical_block_size / BDRV_SECTOR_SIZE) - 1;
    bdrv_guess_geometry(s->bs, &cylinders, &heads, &secs);

    /* NB: per existing s/n string convention the string is terminated
     * by '\0' only when less than sizeof (s->sn)
     */
    dinfo = drive_get_by_blockdev(s->bs);
    strncpy(s->sn, dinfo->serial, sizeof (s->sn));

    s->vq = virtio_add_queue(&s->vdev, 128, virtio_blk_handle_output);

    bdrv_set_removable(s->bs, 0);

    return &s->vdev;
}
