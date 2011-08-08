#ifndef __QEMU_THREAD_POSIX_H
#define __QEMU_THREAD_POSIX_H 1
#include "pthread.h"

struct QemuMutex {
    pthread_mutex_t lock;
};

struct QemuCond {
    pthread_cond_t cond;
};

struct QemuEvent {
#ifndef __linux__
    pthread_mutex_t lock;
    pthread_cond_t cond;
#endif
    unsigned value;
};

#define QEMU_ONCE_INIT { .once = PTHREAD_ONCE_INIT }
struct QemuOnce {
    pthread_once_t once;
};

struct QemuThread {
    pthread_t thread;
};

#endif
