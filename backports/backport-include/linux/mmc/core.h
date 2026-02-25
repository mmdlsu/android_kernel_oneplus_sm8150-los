#ifndef __BACKPORT_LINUX_MMC_CORE_H
#define __BACKPORT_LINUX_MMC_CORE_H

#include_next <linux/mmc/core.h>
#include <linux/version.h>

/*
 * In kernels < 5.18/5.19, mmc_hw_reset/mmc_sw_reset take struct mmc_host*,
 * but backported callers pass struct mmc_card*. We redefine these as macros
 * that dereference card->host at the call site where struct mmc_card is
 * fully defined, avoiding the circular include with mmc/card.h.
 */
#if LINUX_VERSION_IS_LESS(5,19,0) && LINUX_VERSION_IS_GEQ(4,18,0)
#define backport_mmc_sw_reset(card) mmc_sw_reset((card)->host)
#undef mmc_sw_reset
#define mmc_sw_reset(card) backport_mmc_sw_reset(card)
#endif

#if LINUX_VERSION_IS_LESS(5,18,0)
#define backport_mmc_hw_reset(card) mmc_hw_reset((card)->host)
#undef mmc_hw_reset
#define mmc_hw_reset(card) backport_mmc_hw_reset(card)
#endif

#endif
