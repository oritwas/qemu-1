/*
 * Wrappers around mutex/cond/thread functions
 *
 * Copyright Red Hat, Inc. 2009
 *
 * Author:
 *  Marcelo Tosatti <mtosatti@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <linux/futex.h>
#endif
#include "qemu-thread.h"
#include "qemu-common.h"
#include "qemu-tls.h"
#include "qemu/atomic.h"

static void error_exit(int err, const char *msg)
{
    fprintf(stderr, "qemu: %s: %s\n", msg, strerror(err));
    abort();
}

void qemu_mutex_init(QemuMutex *mutex)
{
    int err;
    pthread_mutexattr_t mutexattr;

    pthread_mutexattr_init(&mutexattr);
    pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_ERRORCHECK);
    err = pthread_mutex_init(&mutex->lock, &mutexattr);
    pthread_mutexattr_destroy(&mutexattr);
    if (err)
        error_exit(err, __func__);
}

void qemu_mutex_destroy(QemuMutex *mutex)
{
    int err;

    err = pthread_mutex_destroy(&mutex->lock);
    if (err)
        error_exit(err, __func__);
}

void qemu_mutex_lock(QemuMutex *mutex)
{
    int err;

    err = pthread_mutex_lock(&mutex->lock);
    if (err)
        error_exit(err, __func__);
}

int qemu_mutex_trylock(QemuMutex *mutex)
{
    return pthread_mutex_trylock(&mutex->lock);
}

void qemu_mutex_unlock(QemuMutex *mutex)
{
    int err;

    err = pthread_mutex_unlock(&mutex->lock);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_init(QemuCond *cond)
{
    int err;

    err = pthread_cond_init(&cond->cond, NULL);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_destroy(QemuCond *cond)
{
    int err;

    err = pthread_cond_destroy(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_signal(QemuCond *cond)
{
    int err;

    err = pthread_cond_signal(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_broadcast(QemuCond *cond)
{
    int err;

    err = pthread_cond_broadcast(&cond->cond);
    if (err)
        error_exit(err, __func__);
}

void qemu_cond_wait(QemuCond *cond, QemuMutex *mutex)
{
    int err;

    err = pthread_cond_wait(&cond->cond, &mutex->lock);
    if (err)
        error_exit(err, __func__);
}

#ifdef __linux__
#define futex(...)              syscall(__NR_futex, __VA_ARGS__)

static inline void futex_wake(QemuEvent *ev, int n)
{
    futex(ev, FUTEX_WAKE, n, NULL, NULL, 0);
}

static inline void futex_wait(QemuEvent *ev, unsigned val)
{
    futex(ev, FUTEX_WAIT, (int) val, NULL, NULL, 0);
}
#else
static inline void futex_wake(QemuEvent *ev, int n)
{
  if (n == 1)
    pthread_cond_signal(&ev->cond);
  else
    pthread_cond_broadcast(&ev->cond);
}

static inline void futex_wait(QemuEvent *ev, unsigned val)
{
  pthread_mutex_lock(&ev->lock);
  if (ev->value == val)
    pthread_cond_wait(&ev->cond, &ev->lock);
  pthread_mutex_unlock(&ev->lock);
}
#endif

#define EV_SET		0
#define EV_FREE		1
#define EV_BUSY		-1

void qemu_event_init(QemuEvent *ev, bool init)
{
#ifndef __linux__
    pthread_mutex_init(&ev->lock, NULL);
    pthread_cond_init(&ev->cond, NULL);
#endif

    ev->value = (init ? EV_SET : EV_FREE);
}

void qemu_event_destroy(QemuEvent *ev)
{
#ifndef __linux__
    pthread_mutex_destroy(&ev->lock);
    pthread_cond_destroy(&ev->cond);
#endif
}

void qemu_event_set(QemuEvent *ev)
{
    if (atomic_mb_read(&ev->value) != EV_SET) {
        if (atomic_xchg(&ev->value, EV_SET) == EV_BUSY) {
            /* There were waiters, wake them up.  */
            futex_wake(ev, INT_MAX);
        }
    }
}

void qemu_event_reset(QemuEvent *ev)
{
    if (atomic_mb_read(&ev->value) == EV_SET) {
        /*
         * If there was a concurrent reset (or even reset+wait),
         * do nothing.  Otherwise change EV_SET->EV_FREE.
         */
        atomic_or(&ev->value, EV_FREE);
    }
}

void qemu_event_wait(QemuEvent *ev)
{
    unsigned value;

    value = atomic_mb_read(&ev->value);
    if (value != EV_SET) {
        if (value == EV_FREE) {
            /*
             * Leave the event reset and tell qemu_event_set that there
             * are waiters.  No need to retry, because there cannot be
             * a concurent busy->free transition.  After the CAS, the
             * event will be either set or busy.
             */
            if (atomic_cmpxchg(&ev->value, EV_FREE, EV_BUSY) == EV_SET) {
                return;
            }
        }
        futex_wait(ev, EV_BUSY);
    }
}

void qemu_once(QemuOnce *o, void (*func)(void))
{
    pthread_once(&o->once, func);
}

size_t tls_size;
pthread_key_t tls_key;

static void __attribute__((constructor(102))) tls_init_thread(void)
{
    /* It's easier to always create the key, even if using GCC tls.  */
    pthread_key_create(&tls_key, NULL);
    _tls_init_thread();
}

size_t tls_init(size_t size, size_t alignment)
{
    return _tls_init(size, alignment);
}

typedef struct QemuThreadData {
    void *(*start_routine)(void *);
    void *arg;
} QemuThreadData;

static void *start_routine_wrapper(void *arg)
{
    QemuThreadData args = *(QemuThreadData *) arg;
    qemu_free(arg);
    _tls_init_thread();
    return args.start_routine(args.arg);
}

void qemu_thread_create(QemuThread *thread,
                       void *(*start_routine)(void*),
                       void *arg, int mode)
{
    sigset_t set, oldset;
    QemuThreadData *args = qemu_malloc(sizeof(QemuThreadData));
    int err;
    pthread_attr_t attr;

    err = pthread_attr_init(&attr);
    if (err) {
        error_exit(err, __func__);
    }
    if (mode == QEMU_THREAD_DETACHED) {
        err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (err) {
            error_exit(err, __func__);
        }
    }

    args->start_routine = start_routine;
    args->arg = arg;

    /* Leave signal handling to the iothread.  */
    sigfillset(&set);
    pthread_sigmask(SIG_SETMASK, &set, &oldset);
    err = pthread_create(&thread->thread, &attr, start_routine_wrapper, args);
    if (err)
        error_exit(err, __func__);

    pthread_sigmask(SIG_SETMASK, &oldset, NULL);

    pthread_attr_destroy(&attr);
}

void qemu_thread_get_self(QemuThread *thread)
{
    thread->thread = pthread_self();
}

int qemu_thread_is_self(QemuThread *thread)
{
   return pthread_equal(pthread_self(), thread->thread);
}

void qemu_thread_exit(void *retval)
{
    pthread_exit(retval);
}

void *qemu_thread_join(QemuThread *thread)
{
    int err;
    void *ret;

    err = pthread_join(thread->thread, &ret);
    if (err) {
        error_exit(err, __func__);
    }
    return ret;
}
