// SPDX-FileCopyrightText: 2019 Laszlo Ashin <laszlo@ashin.hu>
//
// SPDX-License-Identifier: GPL-2.0-only

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

#include "util.h"
#include "swupdate_crypto.h"
#include "swupdate_mbedtls.h"

#define MODNAME	"mbedtlsSHA256"

static swupdate_HASH_lib hash;

static char *algo_upper(const char *algo)
{
	static char result[16];
	unsigned i;

	for (i = 0; algo[i] && (i < sizeof(result) - 1); ++i) {
		result[i] = toupper(algo[i]);
	}
	result[i] = '\0';
	return result;
}

static void *mbedtls_HASH_init(const char *algo)
{
	struct mbedtls_digest *dgst;
	int error;

	const mbedtls_md_info_t *info = mbedtls_md_info_from_string(algo_upper(algo));
	if (!info) {
		ERROR("mbedtls_md_info_from_string(\"%s\")", algo);
		return NULL;
	}

	dgst = calloc(1, sizeof(*dgst));
	if (!dgst) {
		return NULL;
	}

	mbedtls_md_init(&dgst->mbedtls_md_context);

	error = mbedtls_md_setup(&dgst->mbedtls_md_context, info, 0);
	if (error) {
		ERROR("mbedtls_md_setup: %d", error);
		goto fail;
	}

	error = mbedtls_md_starts(&dgst->mbedtls_md_context);
	if (error) {
		ERROR("mbedtls_md_starts: %d", error);
		goto fail;
	}

	return dgst;

fail:
	free(dgst);
	return 0;
}

static int mbedtls_HASH_update(void *ctx, const unsigned char *buf,
				size_t len)
{
	struct mbedtls_digest *dgst = (struct mbedtls_digest *)ctx;
	if (!dgst) {
		return -EFAULT;
	}

	const int error = mbedtls_md_update(&dgst->mbedtls_md_context, buf, len);
	if (error) {
		ERROR("mbedtls_md_update: %d", error);
		return -EIO;
	}

	return 0;
}

static int mbedtls_HASH_final(void *ctx, unsigned char *md_value,
		unsigned int *md_len)
{
	struct mbedtls_digest *dgst = (struct mbedtls_digest *)ctx;
	if (!dgst) {
		return -EFAULT;
	}

	int error = mbedtls_md_finish(&dgst->mbedtls_md_context, md_value);
	if (error) {
		return -EINVAL;
	}
	if (md_len) {
#if MBEDTLS_VERSION_NUMBER >= 0x03020000
		*md_len = mbedtls_md_get_size(mbedtls_md_info_from_ctx(&dgst->mbedtls_md_context));
#else
		*md_len = mbedtls_md_get_size(dgst->mbedtls_md_context.md_info);
#endif
	}
	return 1;

}

static void mbedtls_HASH_cleanup(void *ctx)
{
	struct mbedtls_digest *dgst = (struct mbedtls_digest *)ctx;
	if (!dgst) {
		return;
	}

	mbedtls_md_free(&dgst->mbedtls_md_context);
	free(dgst);
}

/*
 * Just a wrap function to memcmp
 */
static int mbedtls_HASH_compare(const unsigned char *hash1, const unsigned char *hash2)
{
	return memcmp(hash1, hash2, SHA256_HASH_LENGTH) ? -1 : 0;
}

__attribute__((constructor))
static void openssl_hash(void)
{
	hash.HASH_init = mbedtls_HASH_init;
	hash.HASH_update = mbedtls_HASH_update;
	hash.HASH_final = mbedtls_HASH_final;
	hash.HASH_compare = mbedtls_HASH_compare;
	hash.HASH_cleanup = mbedtls_HASH_cleanup;
	(void)register_hashlib(MODNAME, &hash);
}
