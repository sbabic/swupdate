# Copyright (C) 2014-2018 Stefano Babic <sbabic@denx.de>
#
# SPDX-License-Identifier:     GPL-2.0-only

lib-$(CONFIG_DOWNLOAD)		+= downloader.o
lib-$(CONFIG_MTD)		+= mtd-interface.o
lib-$(CONFIG_LUA)		+= lua_interface.o lua_compat.o
ifeq ($(CONFIG_SSL_IMPL_OPENSSL)$(CONFIG_SSL_IMPL_WOLFSSL),y)
lib-$(CONFIG_HASH_VERIFY)	+= verify_signature.o
ifeq ($(CONFIG_PKCS11),y)
lib-$(CONFIG_ENCRYPTED_IMAGES)	+= swupdate_decrypt_pkcs11.o
else
lib-$(CONFIG_ENCRYPTED_IMAGES)	+= swupdate_decrypt.o
endif
lib-$(CONFIG_SIGALG_RAWRSA)	+= swupdate_rsa_verify.o
lib-$(CONFIG_SIGALG_RSAPSS)	+= swupdate_rsa_verify.o
endif
ifeq ($(CONFIG_SSL_IMPL_OPENSSL),y)
# wolfSSL does not support CMS in the compatibility layer yet
lib-$(CONFIG_SIGALG_CMS)	+= swupdate_cms_verify.o
endif
ifeq ($(CONFIG_SSL_IMPL_MBEDTLS),y)
lib-$(CONFIG_HASH_VERIFY)	+= verify_signature_mbedtls.o
ifeq ($(CONFIG_PKCS11),y)
lib-$(CONFIG_ENCRYPTED_IMAGES)	+= swupdate_decrypt_pkcs11.o
else
lib-$(CONFIG_ENCRYPTED_IMAGES)	+= swupdate_decrypt_mbedtls.o
endif
lib-$(CONFIG_SIGALG_RAWRSA)	+= swupdate_rsa_verify_mbedtls.o
lib-$(CONFIG_SIGALG_RSAPSS)	+= swupdate_rsa_verify_mbedtls.o
endif
lib-$(CONFIG_LIBCONFIG)		+= swupdate_settings.o \
				   parsing_library_libconfig.o
lib-$(CONFIG_JSON)		+= parsing_library_libjson.o
lib-$(CONFIG_CHANNEL_CURL)	+= channel_curl.o
