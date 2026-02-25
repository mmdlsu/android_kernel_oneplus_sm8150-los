/* SPDX-License-Identifier: GPL-2.0 */
/*
 * qcacld_backport_undo.h — included AFTER backport/backport.h
 *
 * The backport-include headers rename several kernel functions via
 * #define to redirect them to backport_* wrappers with different
 * argument counts or return types.  QCACLD was written for the 4.14
 * API, so we undo the renames that change the function signature.
 *
 * Renames that only add features (e.g. null checks, NLA_F_NESTED
 * flag, return-value wrappers) are left in place.
 */
#ifndef __QCACLD_BACKPORT_UNDO_H
#define __QCACLD_BACKPORT_UNDO_H

/*
 * netif_napi_add: backport redefines it with 3 args (drops weight param).
 * QCACLD uses the original 4-arg form.
 */
#ifdef netif_napi_add
#undef netif_napi_add
#endif

/*
 * netif_napi_add_weight: backport redefines it.
 * Undo so the original (if it exists) or the call compiles normally.
 */
#ifdef netif_napi_add_weight
#undef netif_napi_add_weight
#endif

/*
 * dev_open: backport changes from 1-arg to 2-arg (adds struct netlink_ext_ack *).
 * QCACLD doesn't use dev_open, but undo defensively.
 */
#ifdef dev_open
#undef dev_open
#endif

#endif /* __QCACLD_BACKPORT_UNDO_H */
