/*
 * Simple interface for atomic operations.
 *
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * Author: Paolo Bonzini <pbonzini@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef __QEMU_ATOMIC_H
#define __QEMU_ATOMIC_H 1

/* Compiler barrier */
#define barrier()   ({ asm volatile("" ::: "memory"); (void)0; })

#if defined(__i386__) || defined(__x86_64__)

/*
 * Because of the strongly ordered x86 storage model, wmb() is a nop
 * on x86(well, a compiler barrier only).  Well, at least as long as
 * qemu doesn't do accesses to write-combining memory or non-temporal
 * load/stores from C code.
 */
#define smp_rmb()   barrier()
#define smp_wmb()   barrier()

/*
 * __sync_lock_test_and_set() is documented to be an acquire barrier only,
 * but it is a full barrier at the hardware level.  Add a compiler barrier
 * to make it a full barrier also at the compiler level.
 */
#define atomic_xchg(ptr, i)    (barrier(), __sync_lock_test_and_set(ptr, i))

/*
 * seqcst load/store.
 */
#define atomic_mb_read(ptr)    atomic_read(ptr)
#define atomic_mb_set(ptr, i)  atomic_xchg(ptr, i)

#elif defined(_ARCH_PPC)

/*
 * We use an eieio() for a wmb() on powerpc.  This assumes we don't
 * need to order cacheable and non-cacheable stores with respect to
 * each other
 */
#define smp_rmb()   ({ asm volatile("lwsync" ::: "memory"); (void)0; })
#define smp_wmb()   ({ asm volatile("eieio" ::: "memory"); (void)0; })

#endif

/*
 * For (host) platforms we don't have explicit barrier definitions
 * for, we use the gcc __sync_synchronize() primitive to generate a
 * full barrier.  This should be safe on all platforms, though it may
 * be overkill.
 */
#ifndef smp_wmb
#define smp_wmb()   __sync_synchronize()
#endif

#ifndef smp_mb
#define smp_mb()    __sync_synchronize()
#endif

#ifdef __alpha__
#define smp_read_barrier_depends()   asm volatile("mb":::"memory")
#else
#define smp_read_barrier_depends()
#endif

#ifndef atomic_read
#define atomic_read(ptr)       (*(__typeof__(*ptr) *volatile) (ptr))
#endif

#ifndef atomic_mb_read
#define atomic_mb_read(ptr)    (smp_mb(), atomic_read(ptr), smp_mb())
#endif

#ifndef atomic_set
#define atomic_set(ptr, i)     ((*(__typeof__(*ptr) * volatile) (ptr)) = (i))
#endif

#ifndef atomic_mb_set
#define atomic_mb_set(ptr)     (smp_mb(), atomic_set(ptr, i))
#endif

/*
 * __sync_lock_test_and_set() is documented to be an acquire barrier only.
 */
#ifndef atomic_xchg
#define atomic_xchg(ptr, i)    (smp_mb(), __sync_lock_test_and_set(ptr, i))
#endif

/* Provide shorter names.  */
#define atomic_inc(ptr)        __sync_fetch_and_add(ptr, 1)
#define atomic_dec(ptr)        __sync_fetch_and_add(ptr, -1)
#define atomic_add(ptr, i)     __sync_fetch_and_add(ptr, i)
#define atomic_sub(ptr, i)     __sync_fetch_and_sub(ptr, -(i))
#define atomic_and(ptr, i)     __sync_fetch_and_and(ptr, i)
#define atomic_or(ptr, i)      __sync_fetch_and_or(ptr, -(i))
#define atomic_cmpxchg         __sync_val_compare_and_swap

#endif
