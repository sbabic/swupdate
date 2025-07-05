/*
 * (C) Copyright 2016
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include "util.h"

/*
 * openSSL is not mandatory
 * Let compile when openSSL is not activated
 */
#if defined(CONFIG_HASH_VERIFY) || defined(CONFIG_ENCRYPTED_IMAGES)

#ifdef CONFIG_PKCS11
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/wc_pkcs11.h>
// Exclude p11-kit's pkcs11.h to prevent conflicting with wolfssl's
#define PKCS11_H 1
#include <p11-kit/uri.h>
#endif

#ifdef CONFIG_SSL_IMPL_OPENSSL
#include <openssl/bio.h>
#include <openssl/objects.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/aes.h>
#include <openssl/opensslv.h>
#include <openssl/cms.h>
#elif defined(CONFIG_SSL_IMPL_WOLFSSL)
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/openssl/bio.h>
#include <wolfssl/openssl/objects.h>
#include <wolfssl/openssl/err.h>
#include <wolfssl/openssl/x509.h>
#include <wolfssl/openssl/x509v3.h>
#include <wolfssl/openssl/pem.h>
#include <wolfssl/openssl/evp.h>
#include <wolfssl/openssl/hmac.h>
#include <wolfssl/openssl/aes.h>
#include <wolfssl/openssl/opensslv.h>
#include <wolfssl/openssl/pkcs7.h>
#endif

#if defined(CONFIG_SSL_IMPL_OPENSSL) || defined(CONFIG_SSL_IMPL_WOLFSSL)

#ifdef CONFIG_SIGALG_CMS

static inline uint32_t SSL_X509_get_extension_flags(X509 *x)
{
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	return x->ex_flags;
#else
	return X509_get_extension_flags(x);
#endif
}

static inline uint32_t SSL_X509_get_extended_key_usage(X509 *x)
{
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	return x->ex_xkusage;
#else
	return X509_get_extended_key_usage(x);
#endif
}

#endif /* CONFIG_SIGALG_CMS */

#ifdef CONFIG_SSL_IMPL_WOLFSSL
#define EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx, len) (1)

#define X509_PURPOSE_CODE_SIGN EXTKEYUSE_CODESIGN
#define SSL_PURPOSE_EMAIL_PROT EXTKEYUSE_EMAILPROT
#else
#if !defined(X509_PURPOSE_CODE_SIGN)
#define X509_PURPOSE_CODE_SIGN (X509_PURPOSE_MAX + 1)
#endif
#define SSL_PURPOSE_EMAIL_PROT X509_PURPOSE_SMIME_SIGN
#endif
#define SSL_PURPOSE_CODE_SIGN  X509_PURPOSE_CODE_SIGN
#define SSL_PURPOSE_DEFAULT SSL_PURPOSE_EMAIL_PROT

struct swupdate_digest {
	EVP_PKEY *pkey;		/* this is used for RSA key */
	EVP_PKEY_CTX *ckey;	/* this is used for RSA key */
	X509_STORE *certs;	/* this is used if CMS is set */
	EVP_MD_CTX *ctx;
#ifdef CONFIG_PKCS11
	unsigned char last_decr[AES_BLOCK_SIZE + 1];
	P11KitUri *p11uri;
	Aes ctxdec;
	Pkcs11Dev pkdev;
	Pkcs11Token pktoken;
#elif OPENSSL_VERSION_NUMBER < 0x10100000L
	EVP_CIPHER_CTX ctxdec;
#else
	EVP_CIPHER_CTX *ctxdec;
#endif
#ifdef CONFIG_SIGALG_GPG
	char *gpg_home_directory;
	bool verbose;
	char *gpgme_protocol;
#endif
};

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define SSL_GET_CTXDEC(dgst) &dgst->ctxdec
#else
#define SSL_GET_CTXDEC(dgst) dgst->ctxdec
#endif

/*
 * This just initialize globally the openSSL
 * library
 * It must be called just once
 */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define swupdate_crypto_init() { \
	do { \
		CRYPTO_malloc_init(); \
		OpenSSL_add_all_algorithms(); \
		ERR_load_crypto_strings(); \
	} while (0); \
}
#else
#define swupdate_crypto_init()
#endif

#elif defined(CONFIG_SSL_IMPL_MBEDTLS)
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/cipher.h>

#define EVP_MAX_BLOCK_LENGTH (16)
#define swupdate_crypto_init()

struct swupdate_digest {
#ifdef CONFIG_HASH_VERIFY
	mbedtls_md_context_t mbedtls_md_context;
#endif /* CONFIG_HASH_VERIFY */
#ifdef CONFIG_SIGNED_IMAGES
	mbedtls_pk_context mbedtls_pk_context;
#endif /* CONFIG_SIGNED_IMAGES */
#ifdef CONFIG_PKCS11
	unsigned char last_decr[AES_BLOCK_SIZE + 1];
	P11KitUri *p11uri;
	Aes ctxdec;
	Pkcs11Dev pkdev;
	Pkcs11Token pktoken;
#elif defined(CONFIG_ENCRYPTED_IMAGES)
	mbedtls_cipher_context_t mbedtls_cipher_context;
#endif /* CONFIG_PKCS11 */
#ifdef CONFIG_SIGALG_GPG
	char *gpg_home_directory;
	int verbose;
	char *gpgme_protocol;
#endif
};

#else /* CONFIG_SSL_IMPL */
#error unknown SSL implementation
#endif /* CONFIG_SSL_IMPL */
#else
#define swupdate_crypto_init()
#endif
