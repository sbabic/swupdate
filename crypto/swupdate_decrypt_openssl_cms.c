/*
 * (C) Copyright 2024-2025
 * Michael Glembotzki, iris-GmbH infrared & intelligent sensors, michael.glembotzki@iris-sensing.com
 *
 * (C) Copyright 2025
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Code mostly taken from openssl examples
 */
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include "swupdate.h"
#include "util.h"
#include "swupdate_crypto.h"
#include "swupdate_openssl.h"

static void openssl_cms_probe(void);
static swupdate_decrypt_lib opensslCMS;

static void *openssl_CMS_DECRYPT_init(unsigned char *key,
				      __attribute__ ((__unused__)) char keylen, 
				      __attribute__ ((__unused__)) unsigned char *iv,
				      __attribute__ ((__unused__)) cipher_t cipher)
{
	struct openssl_digest *dgst = NULL;
	X509 *decrypt_cert = NULL;
	EVP_PKEY *pkey = NULL;
	BIO *tbio = NULL;
	const char *keypair_file = (const char *)key;

	if (!keypair_file) {
		ERROR("A PEM private key is not given !");
		return NULL;
	}

	tbio = BIO_new_file(keypair_file, "r");
	if (!tbio) {
		ERROR("%s cannot be opened", keypair_file);
		goto err;
	}

	dgst = calloc(1, sizeof(*dgst));
	if (!dgst) {
		ERROR("OOM, Cannot create internal crypto structure");
		goto err;
	}

	dgst->cryptbuf = BIO_new(BIO_s_mem());
	if (!dgst->cryptbuf) {
		ERROR("Cannot create openSSL BIO buffer");
		goto err;
	}


	pkey = PEM_read_bio_PrivateKey(tbio, NULL, 0, NULL);
	if (!pkey) {
		ERROR("Decryption key not found");
		goto err;
	}
	dgst->pkey = pkey;
	BIO_reset(tbio);

	/*
	 * Cert is optional, but helps OpenSSL find the right key faster in
	 * large CMS recipient lists.
	 */
	decrypt_cert = PEM_read_bio_X509(tbio, NULL, 0, NULL);
	if (!decrypt_cert) {
		WARN("Decryption cert not found");
	} else {
		dgst->decrypt_cert = decrypt_cert;
	}
	BIO_free(tbio);

	return (dgst);

err:
	if (tbio) 
		BIO_free(tbio);
	if (dgst) {
		if (dgst->cryptbuf)
			BIO_free(dgst->cryptbuf);
		free (dgst);
	}

	return NULL;
}

/*
 * Decrypting CMS requires that the encrypted message is in memory.
 * So store the crypted message, and really do the decryption in the final callback
 */
static int openssl_CMS_DECRYPT_update(void *ctx,
				      __attribute__ ((__unused__)) unsigned char *buf,
				      int *outlen,
				      const unsigned char *cryptbuf,
				      int inlen)
{
	struct openssl_digest *dgst = (struct openssl_digest *)ctx;

	if (BIO_write(dgst->cryptbuf, cryptbuf, inlen) < 0) {
		return -EFAULT;
	}

	/*
	 * important: no plaintext is passed to buf during update()
	 */
	*outlen = 0;

	/*
	 * Return -EAGAIN instead of 0:
	 * 0 would signal EOF and stop the loop, so final() would never be called.
	 * -EAGAIN means: no plaintext yet, try again to fill the dgst->cryptbuf.
	 */
	return -EAGAIN;
}

#define BUFSIZE 16384
static int openssl_CMS_DECRYPT_final(void *ctx, unsigned char *buf,
				int *outlen)
{
	struct openssl_digest *dgst = (struct openssl_digest *)ctx;

	/* Decrypt only once and buffer the result in the internal plain BIO. */
	if (!dgst->plain) {
		CMS_ContentInfo *cms = NULL;
		BIO *out = BIO_new(BIO_s_mem());
		if (!out) {
			ERROR("Cannot create openSSL BIO output buffer");
			return -ENOMEM;
		}

		/* Parse message */
		cms = d2i_CMS_bio(dgst->cryptbuf, NULL);
		if (!cms) {
			ERROR("Cannot parse as DER-encoded CMS blob");
			BIO_free(out);
			return -EFAULT;
		}

		if (!CMS_decrypt(cms, dgst->pkey, dgst->decrypt_cert, NULL, out, 0)) {
			ERROR("Decrypting failed");
			CMS_ContentInfo_free(cms);
			BIO_free(out);
			return -EFAULT;
		}
		CMS_ContentInfo_free(cms);
		BIO_free(dgst->cryptbuf);
		dgst->cryptbuf = NULL;
		dgst->plain = out;
	}

	int n = BIO_read(dgst->plain, buf, BUFSIZE);
	if (n < 0) {
		ERROR("Reading from plain BIO failed");
		return -EFAULT;
	}
	*outlen = n;

	if (BIO_pending(dgst->plain) > 0) {
		/* more plain data is pending, let decrypt_step call final() again */
		return -EAGAIN;
	}

	BIO_free(dgst->plain);
	dgst->plain = NULL;
	return 0;
}

static void openssl_CMS_DECRYPT_cleanup(void *ctx)
{
	struct openssl_digest *dgst = (struct openssl_digest *)ctx;
	if (dgst) {
		EVP_CIPHER_CTX_free(SSL_GET_CTXDEC(dgst));
		if (dgst->cryptbuf)
			BIO_free(dgst->cryptbuf);
		if (dgst->plain)
			BIO_free(dgst->plain);
		free(dgst);
	}

	dgst = NULL;

	return;
}

__attribute__((constructor))
static void openssl_cms_probe(void)
{
	opensslCMS.DECRYPT_init = openssl_CMS_DECRYPT_init;
	opensslCMS.DECRYPT_update = openssl_CMS_DECRYPT_update;
	opensslCMS.DECRYPT_final = openssl_CMS_DECRYPT_final;
	opensslCMS.DECRYPT_cleanup = openssl_CMS_DECRYPT_cleanup;
	(void)register_cryptolib("opensslCMS", &opensslCMS);
}
