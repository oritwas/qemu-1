#ifndef __QEMU_THREAD_H
#define __QEMU_THREAD_H 1

#include <inttypes.h>
#include <stdbool.h>

typedef struct QemuMutex QemuMutex;
typedef struct QemuCond QemuCond;
typedef struct QemuEvent QemuEvent;
typedef struct QemuOnce QemuOnce;
typedef struct QemuThread QemuThread;

#ifdef _WIN32
#include "qemu-thread-win32.h"
#else
#include "qemu-thread-posix.h"
#endif

#define QEMU_THREAD_JOINABLE 0
#define QEMU_THREAD_DETACHED 1

void qemu_mutex_init(QemuMutex *mutex);
void qemu_mutex_destroy(QemuMutex *mutex);
void qemu_mutex_lock(QemuMutex *mutex);
int qemu_mutex_trylock(QemuMutex *mutex);
void qemu_mutex_unlock(QemuMutex *mutex);

#define qemu_mutex_init_global(mutex)                                     \
static void __attribute__((constructor)) qemu_mutex_init_##mutex (void) { \
    qemu_mutex_init(&(mutex));                                            \
}                                                                         \
extern int dummy_mutex_init_##mutex

void qemu_cond_init(QemuCond *cond);
void qemu_cond_destroy(QemuCond *cond);

#define qemu_cond_init_global(cond)                                     \
static void __attribute__((constructor)) qemu_cond_init_##cond (void) { \
    qemu_cond_init(&(cond));                                            \
}                                                                       \
extern int dummy_cond_init_##mutex

/*
 * IMPORTANT: The implementation does not guarantee that pthread_cond_signal
 * and pthread_cond_broadcast can be called except while the same mutex is
 * held as in the corresponding pthread_cond_wait calls!
 */
void qemu_cond_signal(QemuCond *cond);
void qemu_cond_broadcast(QemuCond *cond);
void qemu_cond_wait(QemuCond *cond, QemuMutex *mutex);

void qemu_event_init(QemuEvent *ev, bool init);
void qemu_event_set(QemuEvent *ev);
void qemu_event_reset(QemuEvent *ev);
void qemu_event_wait(QemuEvent *ev);
void qemu_event_destroy(QemuEvent *ev);

#define qemu_event_init_global(event, value)                                     \
static void __attribute__((constructor)) qemu_event_init_##event (void) { \
    qemu_event_init(&(event), (value));                                   \
}                                                                         \
extern int dummy_event_init_##event

void qemu_once(QemuOnce *o, void (*func)(void));

void qemu_thread_create(QemuThread *thread,
                        void *(*start_routine)(void *),
                        void *arg, int mode);
void *qemu_thread_join(QemuThread *thread);
void qemu_thread_get_self(QemuThread *thread);
int qemu_thread_is_self(QemuThread *thread);
void qemu_thread_exit(void *retval);

#endif
