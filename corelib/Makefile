# Copyright (C) 2014-2018 Stefano Babic <sbabic@denx.de>
#
# SPDX-License-Identifier:     GPL-2.0-or-later

lib-y				+= installer.o \
				   network_thread.o \
				   stream_interface.o \
				   progress_thread.o \
				   parsing_library.o \
				   artifacts_versions.o \
				   swupdate_dict.o
lib-$(CONFIG_DOWNLOAD)		+= downloader.o
lib-$(CONFIG_MTD)		+= mtd-interface.o
lib-$(CONFIG_LUA)		+= lua_interface.o lua_compat.o
lib-$(CONFIG_HASH_VERIFY)	+= verify_signature.o
lib-$(CONFIG_ENCRYPTED_IMAGES)	+= swupdate_decrypt.o
lib-$(CONFIG_LIBCONFIG)		+= swupdate_settings.o \
				   parsing_library_libconfig.o
lib-$(CONFIG_JSON)		+= parsing_library_libjson.o
lib-$(CONFIG_CURL)		+= channel_curl.o
