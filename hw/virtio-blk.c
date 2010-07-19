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
#include "hw/dataplane/ioq.h"
#include "kvm.h"

enum {
    SEG_MAX = 126,                  /* maximum number of I/O segments */
    VRING_MAX = SEG_MAX + 2,        /* maximum number of vring descriptors */
    REQ_MAX = VRING_MAX / 2,        /* maximum number of requests in the vring */
};

typedef struct {
    struct iocb iocb;               /* Linux AIO control block */
    unsigned char *status;          /* virtio block status code */
    unsigned int head;              /* vring descriptor index */
} VirtIOBlockRequest;

typedef struct {
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
    EventHandler io_handler;        /* Linux AIO completion handler */
    EventHandler notify_handler;    /* virtqueue notify handler */

    IOQueue ioqueue;                /* Linux AIO queue (should really be per dataplane thread) */
    VirtIOBlockRequest requests[REQ_MAX]; /* pool of requests, managed by the queue */
} VirtIOBlock;

static VirtIOBlock *to_virtio_blk(VirtIODevice *vdev)
{
    return (VirtIOBlock *)vdev;
}

/* Normally the block driver passes down the fd, there's no way to get it from
 * above.
 */
static int get_raw_posix_fd_hack(VirtIOBlock *s)
{
    return *(int*)s->bs->file->opaque;
}

static void complete_request(struct iocb *iocb, ssize_t ret, void *opaque)
{
    VirtIOBlock *s = opaque;
    VirtIOBlockRequest *req = container_of(iocb, VirtIOBlockRequest, iocb);
    int len;

    if (likely(ret >= 0)) {
        *req->status = VIRTIO_BLK_S_OK;
        len = ret;
    } else {
        *req->status = VIRTIO_BLK_S_IOERR;
        len = 0;
    }

    /* According to the virtio specification len should be the number of bytes
     * written to, but for virtio-blk it seems to be the number of bytes
     * transferred plus the status bytes.
     */
    vring_push(&s->vring, req->head, len + sizeof req->status);
}

static void process_request(IOQueue *ioq, struct iovec iov[], unsigned int out_num, unsigned int in_num, unsigned int head)
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

    /*
    fprintf(stderr, "virtio-blk request type=%#x sector=%#lx\n",
            outhdr->type, outhdr->sector);
    */

    /* TODO Linux sets the barrier bit even when not advertised! */
    uint32_t type = outhdr->type & ~VIRTIO_BLK_T_BARRIER;

    if (unlikely(type & ~(VIRTIO_BLK_T_OUT | VIRTIO_BLK_T_FLUSH))) {
        fprintf(stderr, "virtio-blk unsupported request type %#x\n", outhdr->type);
        exit(1);
    }

    struct iocb *iocb;
    switch (type & (VIRTIO_BLK_T_OUT | VIRTIO_BLK_T_FLUSH)) {
    case VIRTIO_BLK_T_IN:
        if (unlikely(out_num != 1)) {
            fprintf(stderr, "virtio-blk invalid read request\n");
            exit(1);
        }
        iocb = ioq_rdwr(ioq, true, &iov[1], in_num - 1, outhdr->sector * 512UL); /* TODO is it always 512? */
        break;

    case VIRTIO_BLK_T_OUT:
        if (unlikely(in_num != 1)) {
            fprintf(stderr, "virtio-blk invalid write request\n");
            exit(1);
        }
        iocb = ioq_rdwr(ioq, false, &iov[1], out_num - 1, outhdr->sector * 512UL); /* TODO is it always 512? */
        break;

    case VIRTIO_BLK_T_FLUSH:
        if (unlikely(in_num != 1 || out_num != 1)) {
            fprintf(stderr, "virtio-blk invalid flush request\n");
            exit(1);
        }

        /* TODO fdsync is not supported by all backends, do it synchronously here! */
        {
            VirtIOBlock *s = container_of(ioq, VirtIOBlock, ioqueue);
            fdatasync(get_raw_posix_fd_hack(s));
            inhdr->status = VIRTIO_BLK_S_OK;
            vring_push(&s->vring, head, sizeof *inhdr);
            virtio_irq(s->vq);
        }
        return;

    default:
        fprintf(stderr, "virtio-blk multiple request type bits set\n");
        exit(1);
    }

    /* Fill in virtio block metadata needed for completion */
    VirtIOBlockRequest *req = container_of(iocb, VirtIOBlockRequest, iocb);
    req->head = head;
    req->status = &inhdr->status;
}

static bool handle_notify(EventHandler *handler)
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

        /*
        fprintf(stderr, "out_num=%u in_num=%u head=%u\n", out_num, in_num, head);
        */

        process_request(&s->ioqueue, iov, out_num, in_num, head);
    }

    /* Submit requests, if any */
    int rc = ioq_submit(&s->ioqueue);
    if (unlikely(rc < 0)) {
        fprintf(stderr, "ioq_submit failed %d\n", rc);
        exit(1);
    }
    return true;
}

static bool handle_io(EventHandler *handler)
{
    VirtIOBlock *s = container_of(handler, VirtIOBlock, io_handler);

    if (ioq_run_completion(&s->ioqueue, complete_request, s) > 0) {
        /* TODO is this thread-safe and can it be done faster? */
        virtio_irq(s->vq);
    }

    /* If there were more requests than iovecs, the vring will not be empty yet
     * so check again.  There should now be enough resources to process more
     * requests.
     */
    if (vring_more_avail(&s->vring)) {
        return handle_notify(&s->notify_handler);
    }

    return true;
}

static void *data_plane_thread(void *opaque)
{
    VirtIOBlock *s = opaque;

    event_poll_run(&s->event_poll);
    return NULL;
}

static void data_plane_start(VirtIOBlock *s)
{
    int i;

    vring_setup(&s->vring, &s->vdev, 0);

    event_poll_init(&s->event_poll);

    /* Set up virtqueue notify */
    if (s->vdev.binding->set_host_notifier(s->vdev.binding_opaque, 0, true) != 0) {
        fprintf(stderr, "virtio-blk failed to set host notifier, ensure -enable-kvm is set\n");
        exit(1);
    }
    event_poll_add(&s->event_poll, &s->notify_handler,
                   virtio_queue_get_host_notifier(s->vq),
                   handle_notify);

    /* Set up ioqueue */
    ioq_init(&s->ioqueue, get_raw_posix_fd_hack(s), REQ_MAX);
    for (i = 0; i < ARRAY_SIZE(s->requests); i++) {
        ioq_put_iocb(&s->ioqueue, &s->requests[i].iocb);
    }
    event_poll_add(&s->event_poll, &s->io_handler, ioq_get_notifier(&s->ioqueue), handle_io);

    qemu_thread_create(&s->data_plane_thread, data_plane_thread, s);

    s->data_plane_started = true;
}

static void data_plane_stop(VirtIOBlock *s)
{
    s->data_plane_started = false;

    /* Tell data plane thread to stop and then wait for it to return */
    event_poll_stop(&s->event_poll);
    pthread_join(s->data_plane_thread.thread, NULL);

    ioq_cleanup(&s->ioqueue);

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

    /*
    fprintf(stderr, "virtio_blk_set_status %#x\n", val);
    */

    if (val & VIRTIO_CONFIG_S_DRIVER_OK) {
        data_plane_start(s);
    } else {
        data_plane_stop(s);
    }
}

static void virtio_blk_reset(VirtIODevice *vdev)
{
    virtio_blk_set_status(vdev, 0);
}

static void virtio_blk_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBlock *s = to_virtio_blk(vdev);

    if (s->data_plane_started) {
        fprintf(stderr, "virtio_blk_handle_output: should never get here, "
                        "data plane thread should process requests\n");
        exit(1);
    }

    /* Linux seems to notify before the driver comes up.  This needs more
     * investigation.  Just use a hack for now.
     */
    virtio_blk_set_status(vdev, VIRTIO_CONFIG_S_DRIVER_OK); /* start the thread */

    /* Now kick the thread */
    uint64_t dummy = 1;
    ssize_t unused __attribute__((unused)) = write(event_notifier_get_fd(virtio_queue_get_host_notifier(s->vq)), &dummy, sizeof dummy);
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
    s->vdev.reset = virtio_blk_reset;
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
