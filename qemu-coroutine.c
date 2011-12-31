/*
 * QEMU coroutines
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Stefan Hajnoczi    <stefanha@linux.vnet.ibm.com>
 *  Kevin Wolf         <kwolf@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "trace.h"
#include "qemu-common.h"
#include "qemu-coroutine.h"
#include "qemu-coroutine-int.h"

Coroutine *qemu_coroutine_create(CoroutineEntry *entry)
{
    Coroutine *co = qemu_coroutine_new();
    co->entry = entry;
    co->canceled = false;
    co->cancelable = 1;
    notifier_list_init(&co->cancel_notifiers);
    return co;
}

Coroutine *qemu_coroutine_create_no_cancel(CoroutineEntry *entry)
{
    Coroutine *co = qemu_coroutine_create(entry);
    co->cancelable = 0;
    return co;
}

static void coroutine_swap(Coroutine *from, Coroutine *to)
{
    CoroutineAction ret;

    ret = qemu_coroutine_switch(from, to, COROUTINE_YIELD);

    switch (ret) {
    case COROUTINE_YIELD:
        return;
    case COROUTINE_TERMINATE:
        trace_qemu_coroutine_terminate(to);
        qemu_coroutine_delete(to);
        return;
    default:
        abort();
    }
}

bool qemu_coroutine_canceled(void)
{
    Coroutine *self = qemu_coroutine_self();

    return self->cancelable > 0 && self->canceled;
}

void qemu_coroutine_set_cancelable(bool cancelable)
{
    Coroutine *self = qemu_coroutine_self();

    assert (!(cancelable && self->cancelable > 0));
    self->cancelable += cancelable ? 1 : -1;
    if (self->canceled && self->cancelable > 0) {
        notifier_list_notify(&self->cancel_notifiers, self);
    }
}

void qemu_coroutine_cancel(Coroutine *co)
{
    if (co->canceled) {
        return;
    }
    co->canceled = true;
    notifier_list_notify(&co->cancel_notifiers, co);
}

void qemu_coroutine_add_cancel_notifier(Coroutine *co, Notifier *n)
{
    notifier_list_add(&co->cancel_notifiers, n);
}

void qemu_coroutine_remove_cancel_notifier(Notifier *n)
{
    notifier_remove(n);
}

void qemu_coroutine_enter(Coroutine *co, void *opaque)
{
    Coroutine *self = qemu_coroutine_self();

    trace_qemu_coroutine_enter(self, co, opaque);

    if (co->caller) {
        fprintf(stderr, "Co-routine re-entered recursively\n");
        abort();
    }

    co->caller = self;
    co->entry_arg = opaque;
    coroutine_swap(self, co);
}

void coroutine_fn qemu_coroutine_yield(void)
{
    Coroutine *self = qemu_coroutine_self();
    Coroutine *to = self->caller;

    trace_qemu_coroutine_yield(self, to);

    if (!to) {
        fprintf(stderr, "Co-routine is yielding to no one\n");
        abort();
    }

    self->caller = NULL;
    coroutine_swap(self, to);
}
