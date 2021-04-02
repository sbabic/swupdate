/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
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
#include "compat.h"
#include "swupdate_verify_private.h"

int dgst_init(struct swupdate_digest *dgst, const EVP_MD *md)
{
	int rc;

	ERR_clear_error();
	rc = EVP_DigestInit_ex(dgst->ctx, md, NULL);
	if (rc != 1) {
		ERROR("EVP_DigestInit_ex failed: %s", ERR_error_string(ERR_get_error(), NULL));
		return -EINVAL; /* failed */
	}

	return 0;
}

struct swupdate_digest *swupdate_HASH_init(const char *SHAlength)
{
	struct swupdate_digest *dgst;
	const EVP_MD *md;
	int ret;

	dgst = calloc(1, sizeof(*dgst));
	if (!dgst) {
		return NULL;
	}

	if ((!SHAlength) || strcmp(SHAlength, "sha1"))
		md = EVP_sha256();
	else
		md = EVP_sha1();

 	dgst->ctx = EVP_MD_CTX_create();
	if(dgst->ctx == NULL) {
		ERROR("EVP_MD_CTX_create failed, error 0x%lx", ERR_get_error());
		free(dgst);
		return NULL;
	}

	ret = dgst_init(dgst, md);
	if (ret) {
		free(dgst);
		return NULL;
	}

	return dgst;
}

int swupdate_HASH_update(struct swupdate_digest *dgst, const unsigned char *buf,
				size_t len)
{
	if (!dgst)
		return -EFAULT;

	if (EVP_DigestUpdate (dgst->ctx, buf, len) != 1)
		return -EIO;

	return 0;
}

int swupdate_HASH_final(struct swupdate_digest *dgst, unsigned char *md_value,
		unsigned int *md_len)
{
	if (!dgst)
		return -EFAULT;

	return EVP_DigestFinal_ex (dgst->ctx, md_value, md_len);

}

void swupdate_HASH_cleanup(struct swupdate_digest *dgst)
{
	if (dgst) {
		EVP_MD_CTX_destroy(dgst->ctx);
		free(dgst);
		dgst = NULL;
	}
}

/*
 * Just a wrap function to memcmp
 */
int swupdate_HASH_compare(const unsigned char *hash1, const unsigned char *hash2)
{
	int i;

	for (i = 0; i < SHA256_HASH_LENGTH; i++)
		if (hash1[i] != hash2[i])
			return -1;

	return 0;
}

int swupdate_dgst_init(struct swupdate_cfg *sw, const char *keyfile)
{
	struct swupdate_digest *dgst;
	int ret;

	/*
	 * Check that it was not called before
	 */
	if (sw->dgst) {
		return -EBUSY;
	}

	dgst = calloc(1, sizeof(*dgst));
	if (!dgst) {
		ret = -ENOMEM;
		goto dgst_init_error;
	}

#if defined(CONFIG_SIGALG_RAWRSA) || defined(CONFIG_SIGALG_RSAPSS)
	/*
	 * Load public key
	 */
	dgst->pkey = load_pubkey(keyfile);
	if (!dgst->pkey) {
		ERROR("Error loading pub key from %s", keyfile);
		ret = -EINVAL;
		goto dgst_init_error;
	}
	dgst->ckey = EVP_PKEY_CTX_new(dgst->pkey, NULL);
	if (!dgst->ckey) {
		ERROR("Error creating context key for %s", keyfile);
		ret = -EINVAL;
		goto dgst_init_error;
	}
#elif defined(CONFIG_SIGALG_CMS)
	/*
	 * Load certificate chain
	 */
	dgst->certs = load_cert_chain(keyfile);
	if (!dgst->certs) {
		ERROR("Error loading certificate chain from %s", keyfile);
		ret = -EINVAL;
		goto dgst_init_error;
	}

	{
		static char code_sign_name[] = "Code signing";
		static char code_sign_sname[] = "codesign";

		if (!X509_PURPOSE_add(X509_PURPOSE_CODE_SIGN, X509_TRUST_EMAIL,
				0, check_code_sign, code_sign_name,
				code_sign_sname, NULL)) {
			ERROR("failed to add code sign purpose");
			ret = -EINVAL;
			goto dgst_init_error;
		}
	}

	if (!X509_STORE_set_purpose(dgst->certs, sw->cert_purpose)) {
		ERROR("failed to set purpose");
		ret = -EINVAL;
		goto dgst_init_error;
	}
#else
	TRACE("public key / cert %s ignored, you need to set SIGALG", keyfile);
#endif

	/*
	 * Create context
	 */
	dgst->ctx = EVP_MD_CTX_create();
	if(dgst->ctx == NULL) {
		ERROR("EVP_MD_CTX_create failed, error 0x%lx", ERR_get_error());
		ret = -ENOMEM;
		goto dgst_init_error;
	}

	sw->dgst = dgst;

	return 0;

dgst_init_error:
	if (dgst)
		free(dgst);

	return ret;
}
