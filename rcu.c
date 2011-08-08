/*
 * urcu-qsbr.c
 *
 * Userspace RCU QSBR library
 *
 * Copyright (c) 2009 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright (c) 2009 Paul E. McKenney, IBM Corporation.
 * Copyright 2011 Red Hat, Inc.
 *
 * Ported to QEMU by Paolo Bonzini  <pbonzini@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * IBM's contributions to this file may be relicensed under LGPLv2 or later.
 */

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include "rcu.h"
#include "qemu-barrier.h"

/*
 * Global grace period counter.  Bit 0 is one if the thread is online.
 * Bits 1 and above are defined in synchronize_rcu/update_counter_and_wait.
 */
#define RCU_GP_ONLINE           (1UL << 0)
#define RCU_GP_CTR              (1UL << 1)

unsigned long rcu_gp_ctr = RCU_GP_ONLINE;

QemuEvent rcu_gp_event;
qemu_mutex_init_global(rcu_gp_lock);

QemuMutex rcu_gp_lock;
qemu_event_init_global(rcu_gp_event, true);

/*
 * Check whether a quiescent state was crossed between the beginning of
 * update_counter_and_wait and now.
 */
static inline int rcu_gp_ongoing(unsigned long *ctr)
{
	unsigned long v;

	v = atomic_read(ctr);
	return v && (v != rcu_gp_ctr);
}

/*
 * Written to only by each individual reader. Read by both the reader and the
 * writers.
 */
DEFINE_TLS(struct rcu_reader_data, _rcu_reader);

typedef QLIST_HEAD(, rcu_reader_data) ThreadList;
static ThreadList registry = QLIST_HEAD_INITIALIZER(registry);

static void update_counter_and_wait(void)
{
	ThreadList qsreaders = QLIST_HEAD_INITIALIZER(qsreaders);
	struct rcu_reader_data *index, *tmp;

	if (sizeof (rcu_gp_ctr) <= 4) {
		/* Switch parity: 0 -> 1, 1 -> 0 */
		atomic_set(&rcu_gp_ctr, rcu_gp_ctr ^ RCU_GP_CTR);
	} else {
		/* Increment current grace period.  */
		atomic_set(&rcu_gp_ctr, rcu_gp_ctr + RCU_GP_CTR);
	}

	barrier();

	/*
	 * Wait for each thread rcu_reader_qs_gp count to become 0.
	 */
	for (;;) {
		/*
		 * We want to be notified of changes made to rcu_gp_ongoing
		 * while we walk the list.
		 */
		qemu_event_reset(&rcu_gp_event);
		QLIST_FOREACH(index, &registry, node) {
			atomic_set(&index->waiting, true);
		}
		smp_mb();

		QLIST_FOREACH_SAFE(index, &registry, node, tmp) {
			if (!rcu_gp_ongoing(&index->ctr)) {
				QLIST_REMOVE(index, node);
				QLIST_INSERT_HEAD(&qsreaders, index, node);
			}
		}

		if (QLIST_EMPTY(&registry)) {
			break;
		}

		/*
		 * Wait for one thread to report a quiescent state and
		 * try again.
		 */
		qemu_event_wait(&rcu_gp_event);
	}
	smp_mb();

	/* put back the reader list in the registry */
	QLIST_SWAP(&registry, &qsreaders, node);
}

/*
 * Using a two-subphases algorithm for architectures with smaller than 64-bit
 * long-size to ensure we do not encounter an overflow bug.
 */

void synchronize_rcu(void)
{
	unsigned long was_online;

	was_online = rcu_reader.ctr;

	/* All threads should read qparity before accessing data structure
	 * where new ptr points to.
	 */

	/*
	 * Mark the writer thread offline to make sure we don't wait for
	 * our own quiescent state. This allows using synchronize_rcu()
	 * in threads registered as readers.
	 *
	 * Also includes a memory barrier.
	 */
	if (was_online) {
		rcu_thread_offline();
	} else {
		smp_mb();
	}

	qemu_mutex_lock(&rcu_gp_lock);

	if (QLIST_EMPTY(&registry))
		goto out;

	if (sizeof(rcu_gp_ctr) <= 4) {
		/*
		 * Wait for previous parity to be empty of readers.
		 */
		update_counter_and_wait();	/* 0 -> 1, wait readers in parity 0 */

		/*
		 * Must finish waiting for quiescent state for parity 0 before
		 * committing next rcu_gp_ctr update to memory. Failure to
		 * do so could result in the writer waiting forever while new
		 * readers are always accessing data (no progress).  Enforce
		 * compiler-order of load rcu_reader ctr before store to
		 * rcu_gp_ctr.
		 */
		barrier();

		/*
		 * Adding a memory barrier which is _not_ formally required,
		 * but makes the model easier to understand. It does not have a
		 * big performance impact anyway, given this is the write-side.
		 */
		smp_mb();
	}

	/*
	 * Wait for previous parity/grace period to be empty of readers.
	 */
	update_counter_and_wait();	/* 1 -> 0, wait readers in parity 1 */
out:
	qemu_mutex_unlock(&rcu_gp_lock);

	if (was_online) {
		/* Also includes a memory barrier.  */
		rcu_thread_online();
	} else {
		/*
		 * Finish waiting for reader threads before letting the old
		 * ptr being freed.
		 */
		smp_mb();
	}
}

void rcu_register_thread(void)
{
	assert(rcu_reader.ctr == 0);

	qemu_mutex_lock(&rcu_gp_lock);
	QLIST_INSERT_HEAD(&registry, &rcu_reader, node);
	qemu_mutex_unlock(&rcu_gp_lock);
	rcu_thread_online();
}

void rcu_unregister_thread(void)
{
	/*
	 * We have to make the thread offline otherwise we end up dealocking
	 * with a waiting writer.
	 */
	rcu_thread_offline();
	qemu_mutex_lock(&rcu_gp_lock);
	QLIST_REMOVE(&rcu_reader, node);
	qemu_mutex_unlock(&rcu_gp_lock);
}
