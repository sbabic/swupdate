/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 *
 * Code mostly taken from openssl examples
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <openssl/opensslv.h>
#include "swupdate.h"
#include "sslapi.h"
#include "util.h"

struct swupdate_digest *swupdate_DECRYPT_init(unsigned char *key, unsigned char *iv, unsigned char *salt)
{
	struct swupdate_digest *dgst;
	int ret;

	if ((key == NULL) || (iv == NULL)) {
		ERROR("no key provided for decryption!");
		return NULL;
	}

	const EVP_CIPHER *cipher = EVP_aes_256_cbc();

	dgst = calloc(1, sizeof(*dgst));
	if (!dgst) {
		return NULL;
	}

	if (salt != NULL) {
		unsigned char dummy_key[EVP_MAX_KEY_LENGTH];
		unsigned char dummy_iv[EVP_MAX_IV_LENGTH];
		unsigned char dummy_pwd[5] = "DUMMY";
		if (!EVP_BytesToKey(cipher, EVP_sha1(), salt,
							dummy_pwd, sizeof(dummy_pwd),
							1,
							(unsigned char *)&dummy_key, (unsigned char *)&dummy_iv)) {
			ERROR("Cannot set salt.");
			free(dgst);
			return NULL;
		}
	}

#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	EVP_CIPHER_CTX_init(&dgst->ctxdec);
#else
	dgst->ctxdec = EVP_CIPHER_CTX_new();
	if (dgst->ctxdec == NULL) {
		ERROR("Cannot initialize cipher context.");
		free(dgst);
		return NULL;
	}
	if (EVP_CIPHER_CTX_reset(dgst->ctxdec) != 1) {
		ERROR("Cannot reset cipher context.");
		EVP_CIPHER_CTX_free(dgst->ctxdec);
		free(dgst);
		return NULL;
	}
#endif

	/*
	 * Check openSSL documentation for return errors
	 */
	ret = EVP_DecryptInit_ex(SSL_GET_CTXDEC(dgst), cipher, NULL, key, iv);
	if (ret != 1) {
		const char *reason = ERR_reason_error_string(ERR_peek_error());
		ERROR("Decrypt Engine not initialized, error 0x%lx, reason: %s", ERR_get_error(),
			reason != NULL ? reason : "unknown");
		free(dgst);
		return NULL;
	}

	return dgst;
}

int swupdate_DECRYPT_update(struct swupdate_digest *dgst, unsigned char *buf, 
				int *outlen, unsigned char *cryptbuf, int inlen)
{
	if (EVP_DecryptUpdate(SSL_GET_CTXDEC(dgst), buf, outlen, cryptbuf, inlen) != 1) {
		const char *reason = ERR_reason_error_string(ERR_peek_error());
		ERROR("Update: Decryption error 0x%lx, reason: %s", ERR_get_error(),
			reason != NULL ? reason : "unknown");
		return -EFAULT;
	}

	return 0;
}

int swupdate_DECRYPT_final(struct swupdate_digest *dgst, unsigned char *buf,
				int *outlen)
{
	if (!dgst)
		return -EINVAL;

	if (EVP_DecryptFinal_ex(SSL_GET_CTXDEC(dgst), buf, outlen) != 1) {
		const char *reason = ERR_reason_error_string(ERR_peek_error());
		ERROR("Final: Decryption error 0x%lx, reason: %s", ERR_get_error(),
			reason != NULL ? reason : "unknown");
		return -EFAULT;
	}

	return 0;

}

void swupdate_DECRYPT_cleanup(struct swupdate_digest *dgst)
{
	if (dgst) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
		EVP_CIPHER_CTX_cleanup(SSL_GET_CTXDEC(dgst));
#else
		EVP_CIPHER_CTX_free(SSL_GET_CTXDEC(dgst));
#endif
		free(dgst);
		dgst = NULL;
	}
}
