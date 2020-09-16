/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#ifndef _SWUPDATE_SSL_H
#define _SWUPDATE_SSL_H

#include <stdint.h>

#define SHA_DEFAULT	"sha256"

/*
 * openSSL is not mandatory
 * Let compile when openSSL is not activated
 */
#if defined(CONFIG_HASH_VERIFY) || defined(CONFIG_ENCRYPTED_IMAGES) || \
	defined(CONFIG_CHANNEL_CURL_SSL)
#if defined(CONFIG_SSL_IMPL_OPENSSL) || defined(CONFIG_SSL_IMPL_WOLFSSL)
#include <openssl/bio.h>
#include <openssl/objects.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/aes.h>

#ifdef CONFIG_SIGALG_CMS
#if defined(LIBRESSL_VERSION_NUMBER)
#error "LibreSSL does not support CMS, please select RSA PKCS"
#else
#include <openssl/cms.h>
#endif
#endif

#include <openssl/opensslv.h>

#define X509_PURPOSE_CODE_SIGN (X509_PURPOSE_MAX + 1)
#define SSL_PURPOSE_EMAIL_PROT X509_PURPOSE_SMIME_SIGN
#define SSL_PURPOSE_CODE_SIGN  X509_PURPOSE_CODE_SIGN
#define SSL_PURPOSE_DEFAULT SSL_PURPOSE_EMAIL_PROT

struct swupdate_digest {
	EVP_PKEY *pkey;		/* this is used for RSA key */
	EVP_PKEY_CTX *ckey;	/* this is used for RSA key */
	X509_STORE *certs;	/* this is used if CMS is set */
	EVP_MD_CTX *ctx;
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	EVP_CIPHER_CTX ctxdec;
#else
	EVP_CIPHER_CTX *ctxdec;
#endif
};

#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
#define SSL_GET_CTXDEC(dgst) &dgst->ctxdec
#else
#define SSL_GET_CTXDEC(dgst) dgst->ctxdec
#endif

/*
 * This just initialize globally the openSSL
 * library
 * It must be called just once
 */
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
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

static inline uint32_t SSL_X509_get_extension_flags(X509 *x)
{
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	return x->ex_flags;
#else
	return X509_get_extension_flags(x);
#endif
}

static inline uint32_t SSL_X509_get_extended_key_usage(X509 *x)
{
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	return x->ex_xkusage;
#else
	return X509_get_extended_key_usage(x);
#endif
}

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
#ifdef CONFIG_ENCRYPTED_IMAGES
	mbedtls_cipher_context_t mbedtls_cipher_context;
#endif /* CONFIG_ENCRYPTED_IMAGES */
};

#else /* CONFIG_SSL_IMPL */
#error unknown SSL implementation
#endif /* CONFIG_SSL_IMPL */
#else
#define swupdate_crypto_init()
#endif

#if defined(CONFIG_HASH_VERIFY)
struct swupdate_cfg;

int swupdate_dgst_init(struct swupdate_cfg *sw, const char *keyfile);
struct swupdate_digest *swupdate_HASH_init(const char *SHALength);
int swupdate_HASH_update(struct swupdate_digest *dgst, const unsigned char *buf,
				size_t len);
int swupdate_HASH_final(struct swupdate_digest *dgst, unsigned char *md_value,
	       			unsigned int *md_len);
void swupdate_HASH_cleanup(struct swupdate_digest *dgst);
int swupdate_verify_file(struct swupdate_digest *dgst, const char *sigfile,
				const char *file, const char *signer_name);
int swupdate_HASH_compare(const unsigned char *hash1, const unsigned char *hash2);


#else
#define swupdate_dgst_init(sw, keyfile) ( 0 )
#define swupdate_HASH_init(p) ( NULL )
#define swupdate_verify_file(dgst, sigfile, file) ( 0 )
#define swupdate_HASH_update(p, buf, len)	(-1)
#define swupdate_HASH_final(p, result, len)	(-1)
#define swupdate_HASH_cleanup(sw)
#define swupdate_HASH_compare(hash1,hash2)	(0)
#endif

#ifdef CONFIG_ENCRYPTED_IMAGES
struct swupdate_digest *swupdate_DECRYPT_init(unsigned char *key, unsigned char *iv);
int swupdate_DECRYPT_update(struct swupdate_digest *dgst, unsigned char *buf, 
				int *outlen, const unsigned char *cryptbuf, int inlen);
int swupdate_DECRYPT_final(struct swupdate_digest *dgst, unsigned char *buf,
				int *outlen);
void swupdate_DECRYPT_cleanup(struct swupdate_digest *dgst);
#else
/*
 * Note: macro for swupdate_DECRYPT_init is
 * just to avoid compiler warnings
 */
#define swupdate_DECRYPT_init(key, iv) (((key != NULL) | (ivt != NULL)) ? NULL : NULL)
#define swupdate_DECRYPT_update(p, buf, len, cbuf, inlen) (-1)
#define swupdate_DECRYPT_final(p, buf, len) (-1)
#define swupdate_DECRYPT_cleanup(p)
#endif

#ifndef SSL_PURPOSE_DEFAULT
#define SSL_PURPOSE_EMAIL_PROT -1
#define SSL_PURPOSE_CODE_SIGN  -1
#define SSL_PURPOSE_DEFAULT -1
#endif

#endif

