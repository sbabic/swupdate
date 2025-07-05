/*
 * (C) Copyright 2024
 * Stefano Babic, stefano.babic@swupdate.org.
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
#if !defined(NO_INCLUDE_OPENSSL)
#define MODNAME	"opensslSHA256"
#include "swupdate_openssl.h"
#endif
#include "util.h"
#include "compat.h"
#include "swupdate_crypto.h"

static swupdate_HASH_lib hash;

static int dgst_init(struct openssl_digest *dgst, const EVP_MD *md)
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

static void *openssl_HASH_init(const char *SHAlength)
{
	struct openssl_digest *dgst;
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

static int openssl_HASH_update(void *ctx, const unsigned char *buf, size_t len)
{
	struct openssl_digest *dgst = (struct openssl_digest *)ctx;
	if (!dgst)
		return -EFAULT;

	if (EVP_DigestUpdate (dgst->ctx, buf, len) != 1)
		return -EIO;

	return 0;
}

static int openssl_HASH_final(void *ctx, unsigned char *md_value,
		unsigned int *md_len)
{
	struct openssl_digest *dgst = (struct openssl_digest *)ctx;
	if (!dgst)
		return -EFAULT;

	return EVP_DigestFinal_ex (dgst->ctx, md_value, md_len);

}

static void openssl_HASH_cleanup(void *ctx)
{
	struct openssl_digest *dgst = (struct openssl_digest *)ctx;
	if (dgst) {
		EVP_MD_CTX_destroy(dgst->ctx);
		free(dgst);
		dgst = NULL;
	}
}

static int openssl_HASH_compare(const unsigned char *hash1, const unsigned char *hash2)
{
	int i;

	for (i = 0; i < SHA256_HASH_LENGTH; i++)
		if (hash1[i] != hash2[i])
			return -1;

	return 0;
}

__attribute__((constructor))
static void openssl_hash(void)
{
	hash.HASH_init = openssl_HASH_init;
	hash.HASH_update = openssl_HASH_update;
	hash.HASH_final = openssl_HASH_final;
	hash.HASH_compare = openssl_HASH_compare;
	hash.HASH_cleanup = openssl_HASH_cleanup;
	(void)register_hashlib(MODNAME, &hash);
}
