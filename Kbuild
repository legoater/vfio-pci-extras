ccflags-y += -I$(srctree)/drivers/vfio/pci/

# vfio_check_precopy_ioctl() was added in v7.1
ifneq ($(shell grep -sq 'vfio_check_precopy_ioctl' $(srctree)/include/linux/vfio.h && echo y),y)
ccflags-y += -DNO_VFIO_CHECK_PRECOPY_IOCTL
endif

# kzalloc_obj() was added in v6.16; detect it to handle older kernels (RHEL10)
ifneq ($(shell grep -sq 'kzalloc_obj' $(srctree)/include/linux/slab.h && echo y),y)
ccflags-y += -DNO_KZALLOC_OBJ
endif

obj-m  := igb-vfio-pci.o
igb-vfio-pci-y += main.o
