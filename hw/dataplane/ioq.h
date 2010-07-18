#ifndef IO_QUEUE_H
#define IO_QUEUE_H

typedef struct {
    int fd;                         /* file descriptor */
    unsigned int maxreqs;           /* max length of freelist and queue */

    io_context_t io_ctx;            /* Linux AIO context */
    EventNotifier notifier;         /* Linux AIO eventfd */

    /* Requests can complete in any order so a free list is necessary to manage
     * available iocbs.
     */
    struct iocb **freelist;         /* free iocbs */
    unsigned int freelist_idx;

    /* Multiple requests are queued up before submitting them all in one go */
    struct iocb **queue;            /* queued iocbs */
    unsigned int queue_idx;
} IOQueue;

static void ioq_init(IOQueue *ioq, int fd, unsigned int maxreqs)
{
    ioq->fd = fd;
    ioq->maxreqs = maxreqs;

    if (io_setup(maxreqs, &ioq->io_ctx) != 0) {
        fprintf(stderr, "ioq io_setup failed\n");
        exit(1);
    }

    if (event_notifier_init(&ioq->notifier, 0) != 0) {
        fprintf(stderr, "ioq io event notifier creation failed\n");
        exit(1);
    }

    ioq->freelist = qemu_mallocz(sizeof ioq->freelist[0] * maxreqs);
    ioq->freelist_idx = 0;

    ioq->queue = qemu_mallocz(sizeof ioq->queue[0] * maxreqs);
    ioq->queue_idx = 0;
}

static void ioq_cleanup(IOQueue *ioq)
{
    qemu_free(ioq->freelist);
    qemu_free(ioq->queue);

    event_notifier_cleanup(&ioq->notifier);
    io_destroy(ioq->io_ctx);
}

static EventNotifier *ioq_get_notifier(IOQueue *ioq)
{
    return &ioq->notifier;
}

static struct iocb *ioq_get_iocb(IOQueue *ioq)
{
    if (unlikely(ioq->freelist_idx == 0)) {
        fprintf(stderr, "ioq underflow\n");
        exit(1);
    }
    struct iocb *iocb = ioq->freelist[--ioq->freelist_idx];
    ioq->queue[ioq->queue_idx++] = iocb;
}

static __attribute__((unused)) void ioq_put_iocb(IOQueue *ioq, struct iocb *iocb)
{
    if (unlikely(ioq->freelist_idx == ioq->maxreqs)) {
        fprintf(stderr, "ioq overflow\n");
        exit(1);
    }
    ioq->freelist[ioq->freelist_idx++] = iocb;
}

static __attribute__((unused)) void ioq_rdwr(IOQueue *ioq, bool read, struct iovec *iov, unsigned int count, long long offset)
{
    struct iocb *iocb = ioq_get_iocb(ioq);

    if (read) {
        io_prep_preadv(iocb, ioq->fd, iov, count, offset);
    } else {
        io_prep_pwritev(iocb, ioq->fd, iov, count, offset);
    }
    io_set_eventfd(iocb, event_notifier_get_fd(&ioq->notifier));
}

static __attribute__((unused)) void ioq_fdsync(IOQueue *ioq)
{
    struct iocb *iocb = ioq_get_iocb(ioq);

    io_prep_fdsync(iocb, ioq->fd);
    io_set_eventfd(iocb, event_notifier_get_fd(&ioq->notifier));
}

static __attribute__((unused)) int ioq_submit(IOQueue *ioq)
{
    int rc = io_submit(ioq->io_ctx, ioq->queue_idx, ioq->queue);
    ioq->queue_idx = 0; /* reset */
    return rc;
}

#endif /* IO_QUEUE_H */
