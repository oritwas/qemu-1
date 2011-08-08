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

struct QemuThread {
    pthread_t thread;
};

#endif
