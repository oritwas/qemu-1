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

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <linux/virtio_ring.h>
#include "qemu-common.h"
#include "qemu-thread.h"
#include "virtio-blk.h"

enum {
    SEG_MAX = 126, /* maximum number of I/O segments */
};

typedef struct
{
    EventNotifier *notifier;        /* eventfd */
    void (*handler)(void);          /* handler function */
} EventHandler;

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

    struct vring vring;

    int epoll_fd;                   /* epoll(2) file descriptor */
    io_context_t io_ctx;            /* Linux AIO context */
    EventNotifier io_notifier;      /* Linux AIO eventfd */
    EventHandler io_handler;        /* Linux AIO completion handler */
    EventHandler notify_handler;    /* virtqueue notify handler */

    void *phys_mem_zero_host_ptr;   /* host pointer to guest RAM */
} VirtIOBlock;

static VirtIOBlock *to_virtio_blk(VirtIODevice *vdev)
{
    return (VirtIOBlock *)vdev;
}

/* Map target physical address to host address
 */
static inline void *phys_to_host(VirtIOBlock *s, target_phys_addr_t phys)
{
    /* Adjust for 3.6-4 GB PCI memory range */
    if (phys >= 0x100000000) {
        phys -= 0x100000000 - 0xe0000000;
    } else if (phys >= 0xe0000000) {
        fprintf(stderr, "phys_to_host bad physical address in PCI range %#lx\n", phys);
        exit(1);
    }
    return s->phys_mem_zero_host_ptr + phys;
}

/* Setup for cheap target physical to host address conversion
 *
 * This is a hack for direct access to guest memory, we're not really allowed
 * to do this.
 */
static void setup_phys_to_host(VirtIOBlock *s)
{
    target_phys_addr_t len = 4096; /* RAM is really much larger but we cheat */
    s->phys_mem_zero_host_ptr = cpu_physical_memory_map(0, &len, 0);
    if (!s->phys_mem_zero_host_ptr) {
        fprintf(stderr, "setup_phys_to_host failed\n");
        exit(1);
    }
}

/* Map the guest's vring to host memory
 *
 * This is not allowed but we know the ring won't move.
 */
static void map_vring(struct vring *vring, VirtIOBlock *s, VirtIODevice *vdev, int n)
{
    vring->num = virtio_queue_get_num(vdev, n);
    vring->desc = phys_to_host(s, virtio_queue_get_desc_addr(vdev, n));
    vring->avail = phys_to_host(s, virtio_queue_get_avail_addr(vdev, n));
    vring->used = phys_to_host(s, virtio_queue_get_used_addr(vdev, n));

    fprintf(stderr, "virtio-blk vring physical=%#lx desc=%p avail=%p used=%p\n",
            virtio_queue_get_ring_addr(vdev, n),
            vring->desc, vring->avail, vring->used);
}

static void handle_io(void)
{
    fprintf(stderr, "io completion happened\n");
}

static void handle_notify(void)
{
    fprintf(stderr, "virtqueue notify happened\n");
}

static void *data_plane_thread(void *opaque)
{
    VirtIOBlock *s = opaque;
    struct epoll_event event;
    int nevents;
    EventHandler *event_handler;

    /* Signals are masked, EINTR should never happen */

    for (;;) {
        /* Wait for the next event.  Only do one event per call to keep the
         * function simple, this could be changed later. */
        nevents = epoll_wait(s->epoll_fd, &event, 1, -1);
        if (unlikely(nevents != 1)) {
            fprintf(stderr, "epoll_wait failed: %m\n");
            continue; /* should never happen */
        }

        /* Find out which event handler has become active */
        event_handler = event.data.ptr;

        /* Clear the eventfd */
        event_notifier_test_and_clear(event_handler->notifier);

        /* Handle the event */
        event_handler->handler();
    }
    return NULL;
}

static void add_event_handler(int epoll_fd, EventHandler *event_handler)
{
    struct epoll_event event = {
        .events = EPOLLIN,
        .data.ptr = event_handler,
    };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_notifier_get_fd(event_handler->notifier), &event) != 0) {
        fprintf(stderr, "virtio-blk failed to add event handler to epoll: %m\n");
        exit(1);
    }
}

static void data_plane_start(VirtIOBlock *s)
{
    setup_phys_to_host(s);
    map_vring(&s->vring, s, &s->vdev, 0);

    /* Create epoll file descriptor */
    s->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (s->epoll_fd < 0) {
        fprintf(stderr, "epoll_create1 failed: %m\n");
        return; /* TODO error handling */
    }

    if (s->vdev.binding->set_host_notifier(s->vdev.binding_opaque, 0, true) != 0) {
        fprintf(stderr, "virtio-blk failed to set host notifier\n");
        return; /* TODO error handling */
    }

    s->notify_handler.notifier = virtio_queue_get_host_notifier(s->vq),
    s->notify_handler.handler = handle_notify;
    add_event_handler(s->epoll_fd, &s->notify_handler);

    /* Create aio context */
    if (io_setup(SEG_MAX, &s->io_ctx) != 0) {
        fprintf(stderr, "virtio-blk io_setup failed\n");
        return; /* TODO error handling */
    }

    if (event_notifier_init(&s->io_notifier, 0) != 0) {
        fprintf(stderr, "virtio-blk io event notifier creation failed\n");
        return; /* TODO error handling */
    }

    s->io_handler.notifier = &s->io_notifier;
    s->io_handler.handler = handle_io;
    add_event_handler(s->epoll_fd, &s->io_handler);

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

    close(s->epoll_fd);
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

    s->vq = virtio_add_queue(&s->vdev, SEG_MAX + 2, virtio_blk_handle_output);
    s->data_plane_started = false;

    bdrv_set_removable(s->bs, 0);

    return &s->vdev;
}
