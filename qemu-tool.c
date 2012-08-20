/*
 * Compatibility for qemu-img/qemu-nbd
 *
 * Copyright IBM, Corp. 2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu-common.h"
#include "qemu-timer.h"
#include "qemu-log.h"
#include "migration.h"
#include "main-loop.h"
#include "sysemu.h"
#include "qemu_socket.h"
#include "slirp/libslirp.h"

#include <sys/time.h>

const char *qemu_get_vm_name(void)
{
    return NULL;
}

void vm_stop(RunState state)
{
    abort();
}

int64_t cpu_get_clock(void)
{
    return qemu_get_clock_ns(rt_clock);
}

int64_t cpu_get_icount(void)
{
    abort();
}

void qemu_mutex_lock_iothread(void)
{
}

void qemu_mutex_unlock_iothread(void)
{
}

int use_icount;

void qemu_clock_warp(QEMUClock *clock)
{
}

int qemu_init_main_loop(void)
{
    init_clocks();
    init_timer_alarm();
    return main_loop_init();
}

void slirp_update_timeout(uint32_t *timeout)
{
}

void slirp_select_fill(int *pnfds, fd_set *readfds,
                       fd_set *writefds, fd_set *xfds)
{
}

void slirp_select_poll(fd_set *readfds, fd_set *writefds,
                       fd_set *xfds, int select_error)
{
}

void migrate_add_blocker(Error *reason)
{
}

void migrate_del_blocker(Error *reason)
{
}
