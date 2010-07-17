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

#include <libaio.h>
#include "qemu-common.h"
#include "qemu-thread.h"
#include "virtio-blk.h"
#include "hw/dataplane/event-poll.h"
#include "hw/dataplane/vring.h"
#include "kvm.h"

enum {
    SEG_MAX = 126,                  /* maximum number of I/O segments */
    VRING_MAX = SEG_MAX + 2,        /* maximum number of vring descriptors */
    REQ_MAX = VRING_MAX / 2,        /* maximum number of requests in the vring */
};

typedef struct VirtIOBlock
{
    VirtIODevice vdev;
    BlockDriverState *bs;
    VirtQueue *vq;
    BlockConf *conf;
    unsigned short sector_mask;
    char sn[BLOCK_SERIAL_STRLEN];

    bool data_plane_started;
    QemuThread data_plane_thread;

    Vring vring;                    /* virtqueue vring */

    EventPoll event_poll;           /* event poller */
    io_context_t io_ctx;            /* Linux AIO context */
    EventNotifier io_notifier;      /* Linux AIO eventfd */
    EventHandler io_handler;        /* Linux AIO completion handler */
    EventHandler notify_handler;    /* virtqueue notify handler */
} VirtIOBlock;

static VirtIOBlock *to_virtio_blk(VirtIODevice *vdev)
{
    return (VirtIOBlock *)vdev;
}

static void handle_io(EventHandler *handler)
{
    fprintf(stderr, "io completion happened\n");
}

static void process_request(struct iovec iov[], unsigned int out_num, unsigned int in_num)
{
    /* Virtio block requests look like this: */
    struct virtio_blk_outhdr *outhdr; /* iov[0] */
    /* data[]                            ... */
    struct virtio_blk_inhdr *inhdr;   /* iov[out_num + in_num - 1] */

    if (unlikely(out_num == 0 || in_num == 0 ||
                iov[0].iov_len != sizeof *outhdr ||
                iov[out_num + in_num - 1].iov_len != sizeof *inhdr)) {
        fprintf(stderr, "virtio-blk invalid request\n");
        exit(1);
    }

    outhdr = iov[0].iov_base;
    inhdr = iov[out_num + in_num - 1].iov_base;

    fprintf(stderr, "virtio-blk request type=%#x sector=%#lx\n",
            outhdr->type, outhdr->sector);
}

static void handle_notify(EventHandler *handler)
{
    VirtIOBlock *s = container_of(handler, VirtIOBlock, notify_handler);

    /* There is one array of iovecs into which all new requests are extracted
     * from the vring.  Requests are read from the vring and the translated
     * descriptors are written to the iovecs array.  The iovecs do not have to
     * persist across handle_notify() calls because the kernel copies the
     * iovecs on io_submit().
     *
     * Handling io_submit() EAGAIN may require storing the requests across
     * handle_notify() calls until the kernel has sufficient resources to
     * accept more I/O.  This is not implemented yet.
     */
    struct iovec iovec[VRING_MAX];
    struct iovec *iov, *end = &iovec[VRING_MAX];

    /* When a request is read from the vring, the index of the first descriptor
     * (aka head) is returned so that the completed request can be pushed onto
     * the vring later.
     *
     * The number of hypervisor read-only iovecs is out_num.  The number of
     * hypervisor write-only iovecs is in_num.
     */
    unsigned int head, out_num = 0, in_num = 0;

    for (iov = iovec; ; iov += out_num + in_num) {
        head = vring_pop(&s->vring, iov, end, &out_num, &in_num);
        if (head >= vring_get_num(&s->vring)) {
            break; /* no more requests */
        }

        fprintf(stderr, "head=%u out_num=%u in_num=%u\n", head, out_num, in_num);

        process_request(iov, out_num, in_num);
    }
}

static void *data_plane_thread(void *opaque)
{
    VirtIOBlock *s = opaque;

    for (;;) {
        event_poll(&s->event_poll);
    }
    return NULL;
}

static void data_plane_start(VirtIOBlock *s)
{
    vring_setup(&s->vring, &s->vdev, 0);

    event_poll_init(&s->event_poll);

    if (s->vdev.binding->set_host_notifier(s->vdev.binding_opaque, 0, true) != 0) {
        fprintf(stderr, "virtio-blk failed to set host notifier, ensure -enable-kvm is set\n");
        exit(1);
    }

    event_poll_add(&s->event_poll, &s->notify_handler,
                   virtio_queue_get_host_notifier(s->vq),
                   handle_notify);

    /* Create aio context */
    if (io_setup(SEG_MAX, &s->io_ctx) != 0) {
        fprintf(stderr, "virtio-blk io_setup failed\n");
        exit(1);
    }

    if (event_notifier_init(&s->io_notifier, 0) != 0) {
        fprintf(stderr, "virtio-blk io event notifier creation failed\n");
        exit(1);
    }

    event_poll_add(&s->event_poll, &s->io_handler, &s->io_notifier, handle_io);

    qemu_thread_create(&s->data_plane_thread, data_plane_thread, s);

    s->data_plane_started = true;
}

static void data_plane_stop(VirtIOBlock *s)
{
    s->data_plane_started = false;

    /* TODO stop data plane thread */

    event_notifier_cleanup(&s->io_notifier);
    io_destroy(s->io_ctx);

    s->vdev.binding->set_host_notifier(s->vdev.binding_opaque, 0, false);

    event_poll_cleanup(&s->event_poll);
}

static void virtio_blk_set_status(VirtIODevice *vdev, uint8_t val)
{
    VirtIOBlock *s = to_virtio_blk(vdev);

    /* Toggle host notifier only on status change */
    if (s->data_plane_started == !!(val & VIRTIO_CONFIG_S_DRIVER_OK)) {
        return;
    }

    if (val & VIRTIO_CONFIG_S_DRIVER_OK) {
        data_plane_start(s);
    } else {
        data_plane_stop(s);
    }
}

static void virtio_blk_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    fprintf(stderr, "virtio_blk_handle_output: should never get here, "
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
    stl_raw(&blkcfg.seg_max, SEG_MAX);
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
    s->vdev.set_status = virtio_blk_set_status;
    s->bs = conf->bs;
    s->conf = conf;
    s->sector_mask = (s->conf->logical_block_size / BDRV_SECTOR_SIZE) - 1;
    bdrv_guess_geometry(s->bs, &cylinders, &heads, &secs);

    /* NB: per existing s/n string convention the string is terminated
     * by '\0' only when less than sizeof (s->sn)
     */
    dinfo = drive_get_by_blockdev(s->bs);
    strncpy(s->sn, dinfo->serial, sizeof (s->sn));

    s->vq = virtio_add_queue(&s->vdev, VRING_MAX, virtio_blk_handle_output);
    s->data_plane_started = false;

    bdrv_set_removable(s->bs, 0);

    return &s->vdev;
}
