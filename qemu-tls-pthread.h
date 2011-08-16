/*
 * TLS with pthread_getspecific
 *
 * Copyright Red Hat, Inc. 2011
 *
 * Authors:
 *  Paolo Bonzini   <pbonzini@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_TLS_PTHREAD_H
#define QEMU_TLS_PTHREAD_H

#include <pthread.h>

#define DECLARE_TLS(type, x)                                     \
  extern size_t tls_offset__##x;                                 \
  extern type tls_dummy__##x

#define DEFINE_TLS(type, x)                                      \
  size_t tls_offset__##x;                                        \
  static void __attribute__((constructor(101))) tls_init__##x(void) \
  {                                                              \
    tls_offset__##x = tls_init(sizeof(type), __alignof__(type)); \
  }                                                              \
  extern type tls_dummy__##x

extern size_t tls_size;
extern pthread_key_t tls_key;
extern size_t tls_init(size_t size, size_t alignment);

static inline size_t _tls_init(size_t size, size_t alignment)
{
  size_t tls_offset = (tls_size + alignment - 1) & -alignment;
  tls_size += size;
  return tls_offset;
}

static inline void _tls_init_thread(void)
{
  void *mem = tls_size == 0 ? NULL : qemu_mallocz(tls_size);
  pthread_setspecific(tls_key, mem);
}

static inline __attribute__((__const__)) void *_tls_var(size_t offset)
{
  char *base = pthread_getspecific(tls_key);
  return &base[offset];
}

#define tls_var(x) \
  (*(__typeof__(&tls_dummy__##x)) _tls_var(tls_offset__##x))

#endif
