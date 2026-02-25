#ifndef __BACKPORT_TIMEKEEPING_H
#define __BACKPORT_TIMEKEEPING_H
#include <linux/version.h>
#include <linux/types.h>

#include_next <linux/timekeeping.h>

/*
 * ktime_get_boottime_ns() is already provided by this kernel,
 * no backport needed.
 */

/*
 * ktime_get_raw_ts64 is already provided by this kernel.
 * Note: getrawmonotonic64 is a macro for ktime_get_raw_ts64 here,
 * so the old backport caused infinite recursion.
 */

#if LINUX_VERSION_IS_LESS(4,18,0)
extern time64_t ktime_get_boottime_seconds(void);
#endif /* < 4.18 */

#endif /* __BACKPORT_TIMEKEEPING_H */
