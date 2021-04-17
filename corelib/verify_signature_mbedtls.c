// SPDX-FileCopyrightText: 2019 Laszlo Ashin <laszlo@ashin.hu>
//
// SPDX-License-Identifier: GPL-2.0-only

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

#include "sslapi.h"
#include "util.h"

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

struct swupdate_digest *swupdate_HASH_init(const char *algo)
{
	struct swupdate_digest *dgst;
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

int swupdate_HASH_update(struct swupdate_digest *dgst, const unsigned char *buf,
				size_t len)
{
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

int swupdate_HASH_final(struct swupdate_digest *dgst, unsigned char *md_value,
		unsigned int *md_len)
{
	if (!dgst) {
		return -EFAULT;
	}

	int error = mbedtls_md_finish(&dgst->mbedtls_md_context, md_value);
	if (error) {
		return -EINVAL;
	}
	if (md_len) {
		*md_len = mbedtls_md_get_size(dgst->mbedtls_md_context.md_info);
	}
	return 1;

}

void swupdate_HASH_cleanup(struct swupdate_digest *dgst)
{
	if (!dgst) {
		return;
	}

	mbedtls_md_free(&dgst->mbedtls_md_context);
	free(dgst);
}

/*
 * Just a wrap function to memcmp
 */
int swupdate_HASH_compare(const unsigned char *hash1, const unsigned char *hash2)
{
	return memcmp(hash1, hash2, SHA256_HASH_LENGTH) ? -1 : 0;
}

int swupdate_dgst_init(struct swupdate_cfg *sw, const char *keyfile)
{
	struct swupdate_digest *dgst;

	dgst = calloc(1, sizeof(*dgst));
	if (!dgst) {
		return -ENOMEM;
	}

#ifdef CONFIG_SIGNED_IMAGES
	mbedtls_pk_init(&dgst->mbedtls_pk_context);

	int error = mbedtls_pk_parse_public_keyfile(&dgst->mbedtls_pk_context, keyfile);
	if (error) {
		ERROR("mbedtls_pk_parse_public_keyfile: %d", error);
		free(dgst);
		return -EIO;
	}
#endif

	sw->dgst = dgst;
	return 0;
}
