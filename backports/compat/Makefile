ccflags-y += -I$(src) -Wframe-larger-than=1280
ifeq ($(CONFIG_BACKPORT_INTEGRATE),)
obj-m += compat.o
else
obj-y += compat.o
endif
compat-y += main.o
compat-y += cfg80211_compat.o

# Kernel backport compatibility code
compat-$(CONFIG_KERNEL_4_18) += backport-4.18.o
compat-$(CONFIG_KERNEL_4_19) += backport-4.19.o
compat-$(CONFIG_KERNEL_5_2) += backport-5.2.o backport-genetlink.o
compat-$(CONFIG_KERNEL_5_3) += backport-5.3.o
compat-$(CONFIG_KERNEL_5_5) += backport-5.5.o
compat-$(CONFIG_KERNEL_5_9) += backport-5.9.o
compat-$(CONFIG_KERNEL_5_10) += backport-5.10.o
compat-$(CONFIG_KERNEL_5_11) += backport-5.11.o
compat-$(CONFIG_KERNEL_5_13) += backport-5.13.o
compat-$(CONFIG_KERNEL_5_15) += backport-5.15.o
compat-$(CONFIG_KERNEL_6_4) += backport-6.4.o

compat-$(CONFIG_BPAUTO_BUILD_CRYPTO_LIB_ARC4) += lib-crypto-arc4.o
