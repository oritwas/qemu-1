/*
 * call_rcu implementation
 *
 * Copyright 2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright 2011 Red Hat, Inc.
 *
 * Ported to QEMU by Paolo Bonzini  <pbonzini@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 */

#include "qemu-common.h"
#include "qemu-thread.h"
#include "qemu/atomic.h"
#include "rcu.h"

#define RCU_CALL_MIN_SIZE        30

/* Multi-producer, single-consumer queue based on urcu/static/wfqueue.h
 * from liburcu.  Introducing rcu_call_count removes busy-waiting from
 * the dequeue side.
 */

static struct rcu_head dummy;
static struct rcu_head *head = &dummy, **tail = &dummy.next;
static int rcu_call_count;

static QemuEvent rcu_call_ready_event;
qemu_event_init_global(rcu_call_ready_event, false);

static void enqueue(struct rcu_head *node)
{
    struct rcu_head **old_tail;

    node->next = NULL;
    old_tail = atomic_xchg(&tail, &node->next);
    atomic_set(old_tail, node);
}

static struct rcu_head *dequeue(void)
{
    struct rcu_head *node;

retry:
    /* Test for an empty list, which we do not expect.  Note that for
     * the consumer head and tail are always consistent.  The head
     * is consistent because only the consumer reads/writes it.
     * The tail, because it is the first step in the enqueuing.
     * It is only the next pointers that might be inconsistent.  */
    if (head == &dummy && atomic_read(&tail) == &dummy.next) {
        abort();
    }

    /* Since we are the sole consumer, and we excluded the empty case
     * above, the queue will always have at least two nodes (the dummy
     * node, and the one being removed.  So we do not need to update
     * the tail pointer.  Also, we cannot see second, if the head node
     * has NULL in its next pointer, the value is wrong and we need
     * to wait until its enqueuer finishes the update.  */
    node = head;
    head = atomic_read(&node->next);
    assert (head != NULL);

    /* If we dequeued the dummy node, add it back at the end and retry.  */
    if (node == &dummy) {
        enqueue(node);
        goto retry;
    }

    return node;
}

static void *call_rcu_thread(void *opaque)
{
    for (;;) {
        int n;

        /* Heuristically wait for a decent number of callbacks to pile up.
         * Fetch rcu_call_count now, we only must process elements that were
         * added before synchronize_rcu() starts.
         */
        n = atomic_read(&rcu_call_count);
        if (n < RCU_CALL_MIN_SIZE) {
            int tries = 0;
            while (++tries <= 5) {
                qemu_msleep(100);
                qemu_event_reset(&rcu_call_ready_event);
                n = atomic_read(&rcu_call_count);
                if (n >= RCU_CALL_MIN_SIZE) {
                    break;
                }
                qemu_event_wait(&rcu_call_ready_event);
            }
        }

        __sync_fetch_and_add(&rcu_call_count, -n);
        synchronize_rcu();
        while (n-- > 0) {
            struct rcu_head *node = dequeue();
            node->func(node);
        }
    }
    abort();
}

static void call_rcu_init(void)
{
    QemuThread thread;

    qemu_thread_create(&thread, call_rcu_thread, NULL, QEMU_THREAD_DETACHED);
}

void call_rcu(struct rcu_head *node, void (*func)(struct rcu_head *node))
{
    static QemuOnce init = QEMU_ONCE_INIT;
    qemu_once(&init, call_rcu_init);
    node->func = func;
    enqueue(node);

    /* Because we only update rcu_call_count here, the consumer
     * will never dequeue a partially updated HEAD, which still
     * has NULL in its next pointer.
     */
    __sync_fetch_and_add(&rcu_call_count, 1);
    qemu_event_set(&rcu_call_ready_event);
}

void rcu_free(struct rcu_head *head)
{
    g_free(head);
}
