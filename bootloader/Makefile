# Copyright (C) 2014-2018 Stefano Babic <stefano.babic@swupdate.org>
#
# SPDX-License-Identifier:     GPL-2.0-only
#
obj-$(CONFIG_UBOOT)		+= uboot.o
obj-$(CONFIG_BOOTLOADER_NONE)	+= none.o
obj-$(CONFIG_BOOTLOADER_GRUB)	+= grub.o
obj-$(CONFIG_BOOTLOADER_EBG)	+= ebg.o
obj-$(CONFIG_BOOTLOADER_CBOOT)	+= cboot.o

# Make sure none.o is compiled-in as fallback.
ifeq ($(CONFIG_UBOOT),)
ifeq ($(CONFIG_BOOTLOADER_GRUB),)
ifeq ($(CONFIG_BOOTLOADER_EBG),)
obj-y	+= none.o
endif
endif
endif
