# Copyright (C) 2014-2018 Stefano Babic <stefano.babic@swupdate.org>
#
# SPDX-License-Identifier:     GPL-2.0-only

ifeq ($(CONFIG_SSL_IMPL_OPENSSL)$(CONFIG_SSL_IMPL_WOLFSSL),y)
obj-$(CONFIG_HASH_VERIFY)	+= swupdate_HASH_openssl.o
ifeq ($(CONFIG_PKCS11),y)
obj-$(CONFIG_ENCRYPTED_IMAGES)	+= swupdate_decrypt_pkcs11.o
else
obj-$(CONFIG_ENCRYPTED_IMAGES)	+= swupdate_decrypt_openssl.o
endif
obj-$(CONFIG_SIGALG_RAWRSA)	+= swupdate_rsa_verify_openssl.o
obj-$(CONFIG_SIGALG_RSAPSS)	+= swupdate_rsa_verify_openssl.o
endif
ifeq ($(CONFIG_SSL_IMPL_OPENSSL),y)
obj-$(CONFIG_SIGALG_CMS)	+= swupdate_cms_verify_openssl.o
endif
ifeq ($(CONFIG_SSL_IMPL_WOLFSSL),y)
obj-$(CONFIG_SIGALG_CMS)	+= swupdate_pkcs7_verify.o
endif
ifeq ($(CONFIG_SSL_IMPL_MBEDTLS),y)
obj-$(CONFIG_HASH_VERIFY)	+= swupdate_HASH_mbedtls.o
ifeq ($(CONFIG_PKCS11),y)
obj-$(CONFIG_ENCRYPTED_IMAGES)	+= swupdate_decrypt_pkcs11.o
else
obj-$(CONFIG_ENCRYPTED_IMAGES)	+= swupdate_decrypt_mbedtls.o
endif
obj-$(CONFIG_SIGALG_RAWRSA)	+= swupdate_rsa_verify_mbedtls.o
obj-$(CONFIG_SIGALG_RSAPSS)	+= swupdate_rsa_verify_mbedtls.o
endif
obj-$(CONFIG_SIGALG_GPG)	+= swupdate_gpg_verify.o
