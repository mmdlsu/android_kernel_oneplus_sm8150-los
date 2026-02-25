// SPDX-License-Identifier: GPL-2.0
/*
 * cfg80211 legacy-API compat exports for drivers (e.g. QCACLD) that
 * include the in-tree <include/net/cfg80211.h> where these functions are
 * declared as extern.  The backports implement them as static-inline
 * wrappers calling the _khz / _ext variants, so they never produce
 * linkable symbols.  This file provides the linkable versions.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>

/*
 * The backports header <net/cfg80211.h> defines the following functions
 * as static inline.  Rename them so we can define non-inline, exported
 * versions below.
 */
#define ieee80211_get_channel		_bp_inline_ieee80211_get_channel
#define ieee80211_frequency_to_channel	_bp_inline_ieee80211_frequency_to_channel
#define ieee80211_channel_to_frequency	_bp_inline_ieee80211_channel_to_frequency
#define cfg80211_rx_mgmt		_bp_inline_cfg80211_rx_mgmt
#define cfg80211_mgmt_tx_status		_bp_inline_cfg80211_mgmt_tx_status
#define wiphy_rfkill_set_hw_state	_bp_inline_wiphy_rfkill_set_hw_state
#define wiphy_rfkill_stop_polling	_bp_inline_wiphy_rfkill_stop_polling

#include <net/cfg80211.h>

#undef ieee80211_get_channel
#undef ieee80211_frequency_to_channel
#undef ieee80211_channel_to_frequency
#undef cfg80211_rx_mgmt
#undef cfg80211_mgmt_tx_status
#undef wiphy_rfkill_set_hw_state
#undef wiphy_rfkill_stop_polling

/* ------------------------------------------------------------------ */
/*  Legacy cfg80211 API exports                                       */
/* ------------------------------------------------------------------ */

int ieee80211_frequency_to_channel(int freq)
{
	return ieee80211_freq_khz_to_channel(MHZ_TO_KHZ(freq));
}
EXPORT_SYMBOL(ieee80211_frequency_to_channel);

struct ieee80211_channel *
ieee80211_get_channel(struct wiphy *wiphy, int freq)
{
	return ieee80211_get_channel_khz(wiphy, MHZ_TO_KHZ(freq));
}
EXPORT_SYMBOL(ieee80211_get_channel);

bool cfg80211_rx_mgmt(struct wireless_dev *wdev, int freq, int sig_dbm,
		      const u8 *buf, size_t len, u32 flags)
{
	struct cfg80211_rx_info info = {
		.freq	 = MHZ_TO_KHZ(freq),
		.sig_dbm = sig_dbm,
		.buf	 = buf,
		.len	 = len,
		.flags	 = flags,
	};

	return cfg80211_rx_mgmt_ext(wdev, &info);
}
EXPORT_SYMBOL(cfg80211_rx_mgmt);

void cfg80211_mgmt_tx_status(struct wireless_dev *wdev, u64 cookie,
			     const u8 *buf, size_t len, bool ack, gfp_t gfp)
{
	struct cfg80211_tx_status status = {
		.cookie	= cookie,
		.buf	= buf,
		.len	= len,
		.ack	= ack,
	};

	cfg80211_mgmt_tx_status_ext(wdev, &status, gfp);
}
EXPORT_SYMBOL(cfg80211_mgmt_tx_status);

int
ieee80211_channel_to_frequency(int chan, enum nl80211_band band)
{
	return KHZ_TO_MHZ(ieee80211_channel_to_freq_khz(chan, band));
}
EXPORT_SYMBOL(ieee80211_channel_to_frequency);

void wiphy_rfkill_set_hw_state(struct wiphy *wiphy, bool blocked)
{
	wiphy_rfkill_set_hw_state_reason(wiphy, blocked,
					 RFKILL_HARD_BLOCK_SIGNAL);
}
EXPORT_SYMBOL(wiphy_rfkill_set_hw_state);

void wiphy_rfkill_stop_polling(struct wiphy *wiphy)
{
	rfkill_pause_polling(wiphy->rfkill);
}
EXPORT_SYMBOL(wiphy_rfkill_stop_polling);

/*
 * cfg80211_ap_stopped — not present in the backports header at all.
 * The backports use nl80211_send_ap_stopped(wdev) internally.
 * We export that from nl80211.c and call it here.
 */
extern void nl80211_send_ap_stopped(struct wireless_dev *wdev);

void cfg80211_ap_stopped(struct net_device *netdev, gfp_t gfp)
{
	struct wireless_dev *wdev = netdev->ieee80211_ptr;

	if (wdev)
		nl80211_send_ap_stopped(wdev);
}
EXPORT_SYMBOL(cfg80211_ap_stopped);

/* ------------------------------------------------------------------ */
/*  USB compat                                                        */
/* ------------------------------------------------------------------ */

/*
 * bp_usbnet_tx_timeout — compat shim for ndo_tx_timeout on kernels
 * < 5.6 where the callback does not take a txqueue parameter.
 *
 * We deliberately do NOT include <linux/usb/usbnet.h> because the
 * backports version declares usbnet_tx_timeout with the new two-arg
 * signature, while the running kernel (4.14) exports the old one-arg
 * version.
 */
extern void usbnet_tx_timeout(struct net_device *net);

void bp_usbnet_tx_timeout(struct net_device *dev)
{
	usbnet_tx_timeout(dev);
}
EXPORT_SYMBOL_GPL(bp_usbnet_tx_timeout);
