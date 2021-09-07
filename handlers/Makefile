# Copyright (C) 2014-2018 Stefano Babic <sbabic@denx.de>
#
# SPDX-License-Identifier:     GPL-2.0-only
#
# Note: the handlers are inserted automatically
# via a GCC constructor. To work, the linker should
# not drop the handler if it is not called.
# Handler can be called dynamically based
# on the received image type.
obj-y	+= dummy_handler.o
obj-$(CONFIG_ARCHIVE) += archive_handler.o
obj-$(CONFIG_BOOTLOADERHANDLER) += boot_handler.o
obj-$(CONFIG_CFI)	+= flash_handler.o
obj-$(CONFIG_DELTA)	+= delta_handler.o delta_downloader.o zchunk_range.o
obj-$(CONFIG_DISKFORMAT_HANDLER)	+= diskformat_handler.o
obj-$(CONFIG_DISKPART)	+= diskpart_handler.o
obj-$(CONFIG_UNIQUEUUID)	+= uniqueuuid_handler.o
obj-$(CONFIG_CFIHAMMING1)+= flash_hamming1_handler.o
obj-$(CONFIG_LUASCRIPTHANDLER) += lua_scripthandler.o
obj-$(CONFIG_RAW)	+= raw_handler.o
obj-$(CONFIG_RDIFFHANDLER) += rdiff_handler.o
obj-$(CONFIG_READBACKHANDLER) += readback_handler.o
obj-$(CONFIG_REMOTE_HANDLER) += remote_handler.o
obj-$(CONFIG_SHELLSCRIPTHANDLER) += shell_scripthandler.o
obj-$(CONFIG_SSBLSWITCH) += ssbl_handler.o
obj-$(CONFIG_SWUFORWARDER_HANDLER) += swuforward_handler.o swuforward-ws.o
obj-$(CONFIG_UBIVOL)	+= ubivol_handler.o
obj-$(CONFIG_UCFWHANDLER)	+= ucfw_handler.o
