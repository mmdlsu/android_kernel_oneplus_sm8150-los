The files in this directory have been named based on their original position in kernel source, with underscores _ replacing slashes / so as to not create multiple subdirectories,
e.g. include/linux/scatterlist.h -> include_linux_scatterlist.h
They should be copied into kernel sources at the position indicated by their respective names:
~/android_kernel_xiaomi_surya/mt76/patched_headers_4.14:$ cp include_linux_scatterlist.h ../../include/linux/scatterlist.h
