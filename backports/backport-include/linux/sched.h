#ifndef __BACKPORT_LINUX_SCHED_H
#define __BACKPORT_LINUX_SCHED_H

#include_next <linux/sched.h>
#include <linux/version.h>

/*
 * sched_set_fifo_low() is already declared by this kernel,
 * no backport needed.
 */
#endif /* __BACKPORT_LINUX_SCHED_H */
