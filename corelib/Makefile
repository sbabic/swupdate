# Copyright (C) 2014-2018 Stefano Babic <stefano.babic@swupdate.org>
#
# SPDX-License-Identifier:     GPL-2.0-only

lib-y				+= emmc_utils.o \
				   multipart_parser.o \
				   parsing_library_libjson.o \
				   server_utils.o \
				   swupdate_settings.o \
				   parsing_library_libconfig.o
lib-$(CONFIG_DOWNLOAD)		+= downloader.o
lib-$(CONFIG_MTD)		+= mtd-interface.o
lib-$(CONFIG_LUA)		+= lua_interface.o lua_compat.o
lib-$(CONFIG_CHANNEL_CURL)	+= channel_curl.o
