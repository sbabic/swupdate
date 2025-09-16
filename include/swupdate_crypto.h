/*
 * (C) Copyright 2024
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 *
 */

#pragma once
#include <stdbool.h>
#include <swupdate_aes.h>

#define SHA_DEFAULT	"sha256"

/*
 * This just initialize globally the openSSL
 * library
 * It must be called just once
 */
#if defined (OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x10100000L
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

struct swupdate_cfg;

typedef enum {
	CERT_PURPOSE_EMAIL_PROT,
	CERT_PURPOSE_CODE_SIGN,
	CERT_PURPOSE_LAST = CERT_PURPOSE_CODE_SIGN
} ssl_cert_purpose_t;

typedef struct {
	void *(*DECRYPT_init)(unsigned char *key, char keylen, unsigned char *iv, cipher_t cipher);
	int (*DECRYPT_update)(void *ctx, unsigned char *buf, 
				int *outlen, const unsigned char *cryptbuf, int inlen);

	int (*DECRYPT_final)(void *ctx, unsigned char *buf, int *outlen);
	void (*DECRYPT_cleanup)(void *ctx);
} swupdate_decrypt_lib;

typedef struct {
	void *(*HASH_init)(const char *SHAlength);
	int (*HASH_update)(void *ctx, const unsigned char *buf, size_t len);
	int (*HASH_final)(void *ctx, unsigned char *md_value, unsigned int *md_len);
	int (*HASH_compare)(const unsigned char *hash1, const unsigned char *hash2);
	void (*HASH_cleanup)(void *ctx);
} swupdate_HASH_lib;

typedef struct {
	int (*dgst_init)(struct swupdate_cfg *sw, const char *keyfile);
	int (*verify_file)(void *ctx, const char *sigfile, const char *file, const char *signer_name);
} swupdate_dgst_lib;

/*
 * register_cryptolib - register a crypto engine / library
 *
 * @name : cryptolib's name to register.
 * @swupdate_crypto_lib : structure with crypto engine functions
 *
 * Return:
 *   0 on success, -ENOMEM on error.
 */

int register_cryptolib(const char *name, swupdate_decrypt_lib *lib);
int register_hashlib(const char *name, swupdate_HASH_lib *lib);
int register_dgstlib(const char *name, swupdate_dgst_lib *lib);

/*
 * set_cryptolib - set current crypto library
 *
 * @name : cryptolib's name to register.
 *
 * Return:
 *   0 on success, -ENOENT on error.
 */
int set_cryptolib(const char *name);
int set_HASHlib(const char *name);
int set_dgstlib(const char *name);

/*
 * get_cryptolib - return name of current cryptolib
 *
 *
 * Return:
 *   0 on success, NULL on error.
 */
const char* get_cryptolib(void);
const char* get_HASHlib(void);
const char* get_dgstlib(void);

/*
 * print_registered_cryptolib - list supported crypto libraries
 *
 *
 * Return:
 */
void print_registered_cryptolib(void);

struct swupdate_cfg;

int swupdate_dgst_init(struct swupdate_cfg *sw, const char *keyfile);
void *swupdate_HASH_init(const char *SHALength);
int swupdate_HASH_update(void *ctx, const unsigned char *buf,
				size_t len);
int swupdate_HASH_final(void *ctx, unsigned char *md_value,
	       			unsigned int *md_len);
void swupdate_HASH_cleanup(void *ctx);
int swupdate_verify_file(void *ctx, const char *sigfile,
				const char *file, const char *signer_name);
int swupdate_HASH_compare(const unsigned char *hash1, const unsigned char *hash2);

void *swupdate_DECRYPT_init(unsigned char *key, char keylen, unsigned char *iv, cipher_t cipher);
int swupdate_DECRYPT_update(void *ctx, unsigned char *buf, 
				int *outlen, const unsigned char *cryptbuf, int inlen);
int swupdate_DECRYPT_final(void *ctx, unsigned char *buf,
				int *outlen);
void swupdate_DECRYPT_cleanup(void *ctx);
