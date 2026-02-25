ifeq ($(CONFIG_BACKPORT_INTEGRATE),)
# Since 2.6.21, try-run is available, but cc-disable-warning
# was only added later, so we add it here ourselves:
backport-cc-disable-warning = $(call try-run,\
	$(CC) $(KBUILD_CPPFLAGS) $(KBUILD_CFLAGS) -W$(strip $(1)) -c -x c /dev/null -o "$$TMP",-Wno-$(strip $(1)))

NOSTDINC_FLAGS += \
	-I$(M)/backport-include/ \
	-I$(M)/backport-include/uapi \
	-I$(M)/include/ \
	-I$(M)/include/uapi \
	-include $(M)/backport-include/backport/backport.h \
	$(call backport-cc-disable-warning, unused-but-set-variable) \
	-DCONFIG_VERSION=\"$(BACKPORTS_VERSION)\" \
	-DCONFIG_KERNEL_VERSION=\"$(BACKPORTED_KERNEL_VERSION)\" \
	-DCONFIG_KERNEL_NAME=\"$(BACKPORTED_KERNEL_NAME)\" \
	$(BACKPORTS_GIT_TRACKER_DEF) \
	$(CFLAGS)

export backport_srctree = $(M)
else
export BACKPORT_DIR = backports
export backport_srctree = $(BACKPORT_DIR)
NOSTDINC_FLAGS += \
	-I$(srctree)/$(BACKPORT_DIR)/backport-include/ \
	-I$(srctree)/$(BACKPORT_DIR)/backport-include/uapi \
	-I$(srctree)/$(BACKPORT_DIR)/include/ \
	-I$(srctree)/$(BACKPORT_DIR)/include/uapi \
	-include $(srctree)/$(BACKPORT_DIR)/backport-include/backport/backport.h \
	$(CFLAGS)
endif

subdir-ccflags-y := $(call cc-option, -fno-pie) $(call cc-option, -no-pie)

subdir-ccflags-y += -std=gnu11

obj-y += compat/

obj-$(CONFIG_CFG80211) += net/wireless/
obj-$(CONFIG_MAC80211) += net/mac80211/
obj-$(CONFIG_WLAN) += drivers/net/wireless/

# The following are handled by the kernel's own in-tree versions:
# obj-$(CONFIG_QRTR) += net/qrtr/
# obj-$(CONFIG_QCOM_QMI_HELPERS) += drivers/soc/qcom/
# obj-$(CONFIG_MHI_BUS) += drivers/bus/mhi/
# obj-$(CONFIG_SSB) += drivers/ssb/
# obj-$(CONFIG_BCMA) += drivers/bcma/
# obj-$(CONFIG_USB_NET_RNDIS_WLAN) += drivers/net/usb/
# obj-$(CONFIG_USB_WDM) += drivers/usb/class/
# obj-$(CONFIG_USB_USBNET) += drivers/net/usb/
# obj-$(CONFIG_STAGING) += drivers/staging/
