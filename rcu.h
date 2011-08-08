#ifndef _URCU_QSBR_H
#define _URCU_QSBR_H

/*
 * urcu-qsbr.h
 *
 * Userspace RCU QSBR header.
 *
 * LGPL-compatible code should include this header with :
 *
 * #define _LGPL_SOURCE
 * #include <urcu.h>
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
c* IBM's contributions to this file may be relicensed under LGPLv2 or later.
 */

#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>

#include "compiler.h"
#include "rcu-pointer.h"
#include "qemu-tls.h"
#include "qemu-thread.h"
#include "qemu-queue.h"
#include "qemu/atomic.h"

#ifdef __cplusplus
extern "C" {
#endif 

/*
 * Important !
 *
 * Each thread containing read-side critical sections must be registered
 * with rcu_register_thread() before calling rcu_read_lock().
 * rcu_unregister_thread() should be called before the thread exits.
 */

#ifdef DEBUG_RCU
#define rcu_assert(args...)	assert(args)
#else
#define rcu_assert(args...)
#endif

#define RCU_GP_ONLINE		(1UL << 0)
#define RCU_GP_CTR		(1UL << 1)

/*
 * Global quiescent period counter with low-order bits unused.
 * Using a int rather than a char to eliminate false register dependencies
 * causing stalls on some architectures.
 */
extern unsigned long rcu_gp_ctr;

extern QemuEvent rcu_gp_event;

struct rcu_reader_data {
	/* Data used by both reader and synchronize_rcu() */
	unsigned long ctr;
	bool waiting;

	/* Data used for registry */
	QLIST_ENTRY(rcu_reader_data) node;
};

DECLARE_TLS(struct rcu_reader_data, _rcu_reader);
#define rcu_reader tls_var(_rcu_reader)

static inline void rcu_read_lock(void)
{
	rcu_assert(rcu_reader.ctr);
}

static inline void rcu_read_unlock(void)
{
}

static inline void rcu_quiescent_state(void)
{
	smp_mb();
	atomic_set(&rcu_reader.ctr, atomic_read(&rcu_gp_ctr));
	/* write rcu_reader.ctr before read waiting.  */
	smp_mb();
	if (atomic_read(&rcu_reader.waiting)) {
		atomic_set(&rcu_reader.waiting, false);
		qemu_event_set(&rcu_gp_event);
	}
}

static inline void rcu_thread_offline(void)
{
	smp_mb();
	atomic_set(&rcu_reader.ctr, 0);
	/* write rcu_reader.ctr before read waiting.  */
	smp_mb();
	if (atomic_read(&rcu_reader.waiting)) {
		atomic_set(&rcu_reader.waiting, false);
		qemu_event_set(&rcu_gp_event);
	}
}

static inline void rcu_thread_online(void)
{
	smp_mb();	/* Ensure the compiler does not reorder us with mutex */
	atomic_set(&rcu_reader.ctr, rcu_gp_ctr);
	smp_mb();
}

extern void synchronize_rcu(void);

/*
 * Reader thread registration.
 */
extern void rcu_register_thread(void);
extern void rcu_unregister_thread(void);

#ifdef __cplusplus 
}
#endif

#endif /* _URCU_QSBR_H */
