/*
 * TLS with __thread
 *
 * Copyright (c) 2011 Red Hat, Inc
 * Copyright (c) 2011 Linaro Limited
 *
 * Authors:
 *  Paolo Bonzini <pbonzini@redhat.com>
 *  Peter Maydell <peter.maydell@linaro.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef QEMU_TLS_GCC_H
#define QEMU_TLS_GCC_H

#define DECLARE_TLS(type, x) extern DEFINE_TLS(type, x)
#define DEFINE_TLS(type, x)  __thread __typeof__(type) tls__##x
#define tls_var(x)           tls__##x

extern size_t tls_init(size_t size, size_t alignment);
static inline size_t _tls_init(size_t size, size_t alignment) { return 0; }
static inline void _tls_init_thread(void) {}

#endif
