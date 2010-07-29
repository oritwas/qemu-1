#ifndef IOSCHED_H
#define IOSCHED_H

#include "hw/dataplane/ioq.h"

typedef struct {
    unsigned long iocbs;
    unsigned long merges;
    unsigned long sched_calls;
} IOSched;

typedef void MergeFunc(struct iocb *a, struct iocb *b);

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
    const struct iovec *iov = iocb->u.v.vec;
    size_t nbytes = 0;
    size_t i;
    for (i = 0; i < iocb->u.v.nr; i++) {
        nbytes += iov->iov_len;
        iov++;
    }
    return nbytes;
}

static void iosched_init(IOSched *iosched)
{
    memset(iosched, 0, sizeof *iosched);
}

static __attribute__((unused)) void iosched_print_stats(IOSched *iosched)
{
    fprintf(stderr, "iocbs = %lu merges = %lu sched_calls = %lu\n",
            iosched->iocbs, iosched->merges, iosched->sched_calls);
    memset(iosched, 0, sizeof *iosched);
}

static void iosched(IOSched *iosched, struct iocb *unsorted[], unsigned int *count, MergeFunc merge_func)
{
    struct iocb *sorted[*count];
    unsigned int merges = 0;
    unsigned int i, j;

    /*
    if ((++iosched->sched_calls % 1000) == 0) {
        iosched_print_stats(iosched);
    }
    */

    if (!*count) {
        return;
    }

    memcpy(sorted, unsorted, sizeof sorted);
    qsort(sorted, *count, sizeof sorted[0], iocb_cmp);

    unsorted[0] = sorted[0];
    j = 1;
    for (i = 1; i < *count; i++) {
        struct iocb *last = sorted[i - 1];
        struct iocb *cur = sorted[i];

        if (last->aio_lio_opcode == cur->aio_lio_opcode &&
            last->u.c.offset + iocb_nbytes(last) == cur->u.c.offset) {
            merge_func(last, cur);
            merges++;

            unsorted[j - 1] = cur;
        } else {
            unsorted[j++] = cur;
        }
    }

    iosched->merges += merges;
    iosched->iocbs += *count;
    *count = j;
}

#endif /* IOSCHED_H */
