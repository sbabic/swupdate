/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#ifndef _SWUPDATE_SSL_H
#define _SWUPDATE_SSL_H

/*
 * openSSL is not mandatory
 * Let compile when openSSL is not activated
 */
#if defined(CONFIG_HASH_VERIFY) || defined(CONFIG_ENCRYPTED_IMAGES)

#include <openssl/bio.h>
#include <openssl/objects.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/aes.h>
#include <openssl/cms.h>
#include <openssl/opensslv.h>

struct swupdate_digest {
	EVP_PKEY *pkey;		/* this is used for RSA key */
	X509_STORE *certs;	/* this is used if CMS is set */
	EVP_MD_CTX *ctx;
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	EVP_CIPHER_CTX ctxdec;
#else
	EVP_CIPHER_CTX *ctxdec;
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
	} while (0); \
}
#else
#define swupdate_crypto_init()
#endif

#else
#define swupdate_crypto_init()
#define AES_BLOCK_SIZE	16
#endif

#if defined(CONFIG_HASH_VERIFY)
int swupdate_dgst_init(struct swupdate_cfg *sw, const char *keyfile);
struct swupdate_digest *swupdate_HASH_init(void);
int swupdate_HASH_update(struct swupdate_digest *dgst, unsigned char *buf,
				size_t len);
int swupdate_HASH_final(struct swupdate_digest *dgst, unsigned char *md_value,
	       			unsigned int *md_len);
void swupdate_HASH_cleanup(struct swupdate_digest *dgst);
int swupdate_verify_file(struct swupdate_digest *dgst, const char *sigfile,
	       	const char *file);
int swupdate_HASH_compare(unsigned char *hash1, unsigned char *hash2);


#else
#define swupdate_dgst_init(sw, keyfile) ( 0 )
#define swupdate_HASH_init(p) ( NULL )
#define swupdate_verify_file(dgst, sigfile, file) ( 0 )
#define swupdate_HASH_update(p, buf, len)
#define swupdate_HASH_final(p, result, len)
#define swupdate_HASH_cleanup(sw)
#define swupdate_HASH_compare(hash1,hash2)	(0)
#endif

#ifdef CONFIG_ENCRYPTED_IMAGES
struct swupdate_digest *swupdate_DECRYPT_init(unsigned char *key, unsigned char *iv);
int swupdate_DECRYPT_update(struct swupdate_digest *dgst, unsigned char *buf, 
				int *outlen, unsigned char *cryptbuf, int inlen);
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

#endif

