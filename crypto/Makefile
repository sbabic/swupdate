# Copyright (C) 2014-2018 Stefano Babic <stefano.babic@swupdate.org>
#
# SPDX-License-Identifier:     GPL-2.0-only

ifeq ($(CONFIG_SSL_IMPL_OPENSSL),y)
obj-$(CONFIG_HASH_VERIFY)	+= swupdate_HASH_openssl.o
obj-$(CONFIG_SIGALG_RAWRSA)	+= swupdate_rsa_verify_openssl.o
obj-$(CONFIG_SIGALG_RSAPSS)	+= swupdate_rsa_verify_openssl.o
obj-$(CONFIG_SIGALG_CMS)	+= swupdate_cms_verify_openssl.o
obj-$(CONFIG_ENCRYPTED_IMAGES)	+= swupdate_decrypt_openssl.o
obj-$(CONFIG_SIGALG_ASYM_DEC_CMS) += swupdate_decrypt_openssl_cms.o
endif

ifeq ($(CONFIG_SSL_IMPL_WOLFSSL),y)
obj-$(CONFIG_HASH_VERIFY)	+= swupdate_HASH_wolfssl.o
obj-$(CONFIG_SIGALG_RAWRSA)	+= swupdate_rsa_verify_wolfssl.o
obj-$(CONFIG_SIGALG_RSAPSS)	+= swupdate_rsa_verify_wolfssl.o
obj-$(CONFIG_SIGALG_CMS)	+= swupdate_pkcs7_verify_wolfssl.o
obj-$(CONFIG_ENCRYPTED_IMAGES)	+= swupdate_decrypt_wolfssl.o
endif

ifeq ($(CONFIG_SSL_IMPL_MBEDTLS),y)
obj-$(CONFIG_HASH_VERIFY)	+= swupdate_HASH_mbedtls.o
obj-$(CONFIG_ENCRYPTED_IMAGES)	+= swupdate_decrypt_mbedtls.o
obj-$(CONFIG_SIGALG_RAWRSA)	+= swupdate_rsa_verify_mbedtls.o
obj-$(CONFIG_SIGALG_RSAPSS)	+= swupdate_rsa_verify_mbedtls.o
endif
ifeq ($(CONFIG_SSL_IMPL_GPGME),y)
obj-$(CONFIG_SIGALG_GPG)	+= swupdate_gpg_verify.o
endif

ifeq ($(CONFIG_PKCS11),y)
obj-$(CONFIG_ENCRYPTED_IMAGES)	+= swupdate_decrypt_pkcs11_p11kit.o
endif
