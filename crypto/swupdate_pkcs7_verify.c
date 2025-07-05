/*
 * (C) Copyright 2019
 * Stefano Babic, stefano.babic@swupdate.org.
 * (C) Copyright 2023
 * Bastian Germann
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 *
 * Code mostly taken from openssl examples
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "swupdate.h"
#include "sslapi.h"
#include "util.h"
#include "swupdate_verify_private.h"

static int store_verify_callback(int ok, X509_STORE_CTX *ctx) {
	int cert_error = X509_STORE_CTX_get_error(ctx);

	if (!ok) {
		switch (cert_error) {
#if defined(CONFIG_CMS_IGNORE_EXPIRED_CERTIFICATE)
		case X509_V_ERR_CERT_HAS_EXPIRED:
		case X509_V_ERR_CERT_NOT_YET_VALID:
			ok = 1;
			break;
#endif
#if defined(CONFIG_CMS_IGNORE_CERTIFICATE_PURPOSE)
		case X509_V_ERR_INVALID_PURPOSE:
			ok = 1;
			break;
#endif
		default:
			break;
		}
	}

	return ok;
}

X509_STORE *load_cert_chain(const char *file)
{
	X509_STORE *castore = X509_STORE_new();
	if (!castore) {
		return NULL;
	}

	/*
	 * Set error callback function for verification of CRTs and CRLs in order
	 * to ignore some errors depending on configuration
	 */
	X509_STORE_set_verify_cb(castore, store_verify_callback);

	BIO *castore_bio = BIO_new_file(file, "r");
	if (!castore_bio) {
		TRACE("failed: BIO_new_file(%s)", file);
		return NULL;
	}

	int crt_count = 0;
	X509 *crt = NULL;
	do {
		crt = PEM_read_bio_X509(castore_bio, NULL, 0, NULL);
		if (crt) {
			crt_count++;
			char *subj = X509_NAME_oneline(X509_get_subject_name(crt), NULL, 0);
			char *issuer = X509_NAME_oneline(X509_get_issuer_name(crt), NULL, 0);
			TRACE("Read PEM #%d: %s %s", crt_count, issuer, subj);
			free(subj);
			free(issuer);
			if (X509_STORE_add_cert(castore, crt) == 0) {
				TRACE("Adding certificate to X509_STORE failed");
				BIO_free(castore_bio);
				X509_STORE_free(castore);
				return NULL;
			}
		}
	} while (crt);
	BIO_free(castore_bio);

	if (crt_count == 0) {
		X509_STORE_free(castore);
		return NULL;
	}

	return castore;
}

static int check_signer_name(const char *name)
{
	// TODO work around wolfSSL's PKCS7_get0_signers always returning NULL
	if (name)
		WARN("The X.509 common name might not be equal to %s.", name);
	return 0;
}

int swupdate_verify_file(struct swupdate_digest *dgst, const char *sigfile,
		const char *file, const char *signer_name)
{
	int status = -EFAULT;
	WOLFSSL_PKCS7* pkcs7 =  (WOLFSSL_PKCS7 *)PKCS7_new();
	BIO *bio_mem = NULL;
	BIO *content_bio = NULL;

	/* Open detached signature that needs to be checked */
	BIO *sigfile_bio = BIO_new_file(sigfile, "rb");
	if (!sigfile_bio) {
		ERROR("%s cannot be opened", sigfile);
		status = -EBADF;
		goto out;
	}

	pkcs7->len = wolfSSL_BIO_get_len(sigfile_bio);
	pkcs7->data = calloc(1, pkcs7->len);
	BIO_read(sigfile_bio, pkcs7->data, pkcs7->len);
	if (!pkcs7->data) {
		ERROR("%s cannot be parsed as DER-encoded PKCS#7 signature blob", sigfile);
		status = -EFAULT;
		goto out;
	}

	/* Open the content file (data which was signed) */
	content_bio = BIO_new_file(file, "rb");
	if (!content_bio) {
		ERROR("%s cannot be opened", file);
		status = -EBADF;
		goto out;
	}
	long content_len = wolfSSL_BIO_get_len(content_bio);
	char *content = calloc(1, content_len);
	BIO_read(content_bio, content, content_len);
	bio_mem = BIO_new_mem_buf(content, content_len);

	/* Then try to verify signature. The BIO* in parameter has to be a mem BIO.
           See https://github.com/wolfSSL/wolfssl/issues/6174. */
	if (!PKCS7_verify((PKCS7 *)pkcs7, NULL, dgst->certs, bio_mem,
			NULL, PKCS7_BINARY)) {
		ERR_print_errors_fp(stderr);
		ERROR("Signature verification failed");
		status = -EBADMSG;
		goto out;
	}

	if (check_signer_name(signer_name)) {
		ERROR("failed to verify signer name");
		status = -EFAULT;
		goto out;
	}

	TRACE("Verified OK");

	/* Signature is valid */
	status = 0;
out:

	if (pkcs7) {
		PKCS7_free((PKCS7 *)pkcs7);
	}
	if (bio_mem) {
		BIO_free(bio_mem);
	}
	if (content_bio) {
		BIO_free(content_bio);
	}
	if (sigfile_bio) {
		BIO_free(sigfile_bio);
	}
	return status;
}
