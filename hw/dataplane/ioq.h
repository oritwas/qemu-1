#ifndef IO_QUEUE_H
#define IO_QUEUE_H

typedef struct {
    int fd;                         /* file descriptor */
    unsigned int max_reqs;           /* max length of freelist and queue */

    io_context_t io_ctx;            /* Linux AIO context */
    EventNotifier io_notifier;      /* Linux AIO eventfd */

    /* Requests can complete in any order so a free list is necessary to manage
     * available iocbs.
     */
    struct iocb **freelist;         /* free iocbs */
    unsigned int freelist_idx;

    /* Multiple requests are queued up before submitting them all in one go */
    struct iocb **queue;            /* queued iocbs */
    unsigned int queue_idx;
} IOQueue;

static void ioq_init(IOQueue *ioq, int fd, unsigned int max_reqs)
{
    int rc;

    ioq->fd = fd;
    ioq->max_reqs = max_reqs;

    memset(&ioq->io_ctx, 0, sizeof ioq->io_ctx);
    if ((rc = io_setup(max_reqs, &ioq->io_ctx)) != 0) {
        fprintf(stderr, "ioq io_setup failed %d\n", rc);
        exit(1);
    }

    if ((rc = event_notifier_init(&ioq->io_notifier, 0)) != 0) {
        fprintf(stderr, "ioq io event notifier creation failed %d\n", rc);
        exit(1);
    }

    ioq->freelist = qemu_mallocz(sizeof ioq->freelist[0] * max_reqs);
    ioq->freelist_idx = 0;

    ioq->queue = qemu_mallocz(sizeof ioq->queue[0] * max_reqs);
    ioq->queue_idx = 0;
}

static void ioq_cleanup(IOQueue *ioq)
{
    qemu_free(ioq->freelist);
    qemu_free(ioq->queue);

    event_notifier_cleanup(&ioq->io_notifier);
    io_destroy(ioq->io_ctx);
}

static EventNotifier *ioq_get_notifier(IOQueue *ioq)
{
    return &ioq->io_notifier;
}

static struct iocb *ioq_get_iocb(IOQueue *ioq)
{
    if (unlikely(ioq->freelist_idx == 0)) {
        fprintf(stderr, "ioq underflow\n");
        exit(1);
    }
    struct iocb *iocb = ioq->freelist[--ioq->freelist_idx];
    ioq->queue[ioq->queue_idx++] = iocb;
    return iocb;
}

static void ioq_put_iocb(IOQueue *ioq, struct iocb *iocb)
{
    if (unlikely(ioq->freelist_idx == ioq->max_reqs)) {
        fprintf(stderr, "ioq overflow\n");
        exit(1);
    }
    ioq->freelist[ioq->freelist_idx++] = iocb;
}

static struct iocb *ioq_rdwr(IOQueue *ioq, bool read, struct iovec *iov, unsigned int count, long long offset)
{
    struct iocb *iocb = ioq_get_iocb(ioq);

    if (read) {
        io_prep_preadv(iocb, ioq->fd, iov, count, offset);
    } else {
        io_prep_pwritev(iocb, ioq->fd, iov, count, offset);
    }
    io_set_eventfd(iocb, event_notifier_get_fd(&ioq->io_notifier));
    return iocb;
}

static struct iocb *ioq_fdsync(IOQueue *ioq)
{
    struct iocb *iocb = ioq_get_iocb(ioq);

    io_prep_fdsync(iocb, ioq->fd);
    io_set_eventfd(iocb, event_notifier_get_fd(&ioq->io_notifier));
    return iocb;
}

static int ioq_submit(IOQueue *ioq)
{
    int rc = io_submit(ioq->io_ctx, ioq->queue_idx, ioq->queue);
    ioq->queue_idx = 0; /* reset */
    return rc;
}

typedef void IOQueueCompletion(struct iocb *iocb, ssize_t ret, void *opaque);
static int ioq_run_completion(IOQueue *ioq, IOQueueCompletion *completion, void *opaque)
{
    struct io_event events[ioq->max_reqs];
    int nevents, i;

    nevents = io_getevents(ioq->io_ctx, 0, ioq->max_reqs, events, NULL);
    if (unlikely(nevents < 0)) {
        fprintf(stderr, "io_getevents failed %d\n", nevents);
        exit(1);
    }

    for (i = 0; i < nevents; i++) {
        ssize_t ret = ((uint64_t)events[i].res2 << 32) | events[i].res;

        completion(events[i].obj, ret, opaque);
        ioq_put_iocb(ioq, events[i].obj);
    }
    return nevents;
}

#endif /* IO_QUEUE_H */
