#ifndef IOSCHED_H
#define IOSCHED_H

#include "hw/dataplane/ioq.h"

typedef struct {
    unsigned long iocbs;
    unsigned long merges;
    unsigned long sched_calls;
} IOSched;

static int iocb_cmp(const void *a, const void *b)
{
    const struct iocb *iocb_a = a;
    const struct iocb *iocb_b = b;

    /*
     * Note that we can't simply subtract req2->sector from req1->sector
     * here as that could overflow the return value.
     */
    if (iocb_a->u.c.offset > iocb_b->u.c.offset) {
        return 1;
    } else if (iocb_a->u.c.offset < iocb_b->u.c.offset) {
        return -1;
    } else {
        return 0;
    }
}

static size_t iocb_nbytes(struct iocb *iocb)
{
    struct iovec *iov = iocb->u.c.buf;
    size_t nbytes = 0;
    size_t i;
    for (i = 0; i < iocb->u.c.nbytes; i++) {
        nbytes += iov->iov_len;
        iov++;
    }
    return nbytes;
}

static void iosched_init(IOSched *iosched)
{
    memset(iosched, 0, sizeof *iosched);
}

static void iosched_print_stats(IOSched *iosched)
{
    fprintf(stderr, "iocbs = %lu merges = %lu sched_calls = %lu\n",
            iosched->iocbs, iosched->merges, iosched->sched_calls);
    memset(iosched, 0, sizeof *iosched);
}

static void iosched(IOSched *iosched, struct iocb *unsorted[], unsigned int count)
{
    struct iocb *sorted[count];
    struct iocb *last;
    unsigned int i;

    if ((++iosched->sched_calls % 1000) == 0) {
        iosched_print_stats(iosched);
    }

    memcpy(sorted, unsorted, sizeof sorted);
    qsort(sorted, count, sizeof sorted[0], iocb_cmp);

    iosched->iocbs += count;
    last = sorted[0];
    for (i = 1; i < count; i++) {
        if (last->aio_lio_opcode == sorted[i]->aio_lio_opcode &&
            last->u.c.offset + iocb_nbytes(last) == sorted[i]->u.c.offset) {
            iosched->merges++;
        }
        last = sorted[i];
    }
}

#endif /* IOSCHED_H */
