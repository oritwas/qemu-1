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
#include "monitor.h"

Monitor *cur_mon;

int monitor_cur_is_qmp(void)
{
    return 0;
}

void monitor_set_error(Monitor *mon, QError *qerror)
{
}

void monitor_vprintf(Monitor *mon, const char *fmt, va_list ap)
{
}

void monitor_printf(Monitor *mon, const char *fmt, ...)
{
}

void monitor_print_filename(Monitor *mon, const char *filename)
{
}

void monitor_protocol_event(MonitorEvent event, QObject *data)
{
}

int monitor_fdset_get_fd(int64_t fdset_id, int flags)
{
	    return -1;
}

int monitor_fdset_dup_fd_add(int64_t fdset_id, int dup_fd)
{
	    return -1;
}

int monitor_fdset_dup_fd_remove(int dup_fd)
{
	    return -1;
}

int monitor_fdset_dup_fd_find(int dup_fd)
{
	    return -1;
}
