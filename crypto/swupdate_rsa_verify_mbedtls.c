/*
 * (C) Copyright 2019
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "util.h"
#include "swupdate.h"
#include "swupdate_crypto.h"
#include "swupdate_mbedtls.h"

#define MODNAME	"mbedtlsRSA"
#define MODNAME_PSS	"mbedtlsRSAPSS"

static swupdate_dgst_lib	libs;

/*
 * mbedtls_md_file() is not public anymore since mbedTLS 4.0
 */
static int md_file(const mbedtls_md_info_t *md_info, const char *path,
		unsigned char *output)
{
	mbedtls_md_context_t ctx;
	unsigned char buf[4096];
	size_t n;
	FILE *f;
	int error;

	f = fopen(path, "rb");
	if (!f) {
		ERROR("%s cannot be opened", path);
		return -ENOENT;
	}

	mbedtls_md_init(&ctx);
	error = mbedtls_md_setup(&ctx, md_info, 0);
	if (!error)
		error = mbedtls_md_starts(&ctx);
	while (!error && (n = fread(buf, 1, sizeof(buf), f)) > 0)
		error = mbedtls_md_update(&ctx, buf, n);
	if (!error && ferror(f))
		error = -EIO;
	if (!error)
		error = mbedtls_md_finish(&ctx, output);

	mbedtls_md_free(&ctx);
	fclose(f);
	return error;
}

static int mbedtls_rsa_verify_file(void *ctx, const char *sigfile,
		const char *file, const char *signer_name)
{
	struct mbedtls_digest *dgst = (struct mbedtls_digest *)ctx;
	unsigned char *sigbuf = NULL;
	size_t sigbuf_len = 0;
	int error;
	uint8_t hash_computed[32];
	const mbedtls_md_info_t *md_info;
	bool pss = get_dgstlib() && !strcmp(get_dgstlib(), MODNAME_PSS);
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
	/* PSS uses MGF1 with the same hash and accepts any salt length */
	mbedtls_pk_sigalg_t sigalg = pss ? MBEDTLS_PK_SIGALG_RSA_PSS :
		MBEDTLS_PK_SIGALG_RSA_PKCS1V15;
#else
	mbedtls_pk_type_t pk_type = MBEDTLS_PK_RSA;
	void *pss_options = NULL;
	mbedtls_pk_rsassa_pss_options options = {
		.mgf1_hash_id = MBEDTLS_MD_SHA256,
		.expected_salt_len = MBEDTLS_RSA_SALT_LEN_ANY
	};
	if (pss) {
		pk_type = MBEDTLS_PK_RSASSA_PSS;
		pss_options = &options;
	}
#endif

	(void)signer_name;

	md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	if (!md_info) {
		ERROR("mbedtls_md_info_from_type");
		return -ENOENT;
	}

	assert(mbedtls_md_get_size(md_info) == sizeof(hash_computed));

	error = md_file(md_info, file, hash_computed);
	if (error) {
		ERROR("md_file: %d", error);
		return error;
	}

	error = read_file_into_buf(sigfile, &sigbuf, &sigbuf_len);
	if (error) {
		return error;
	}

	int ret = mbedtls_pk_verify_ext(
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
		sigalg,
#else
		pk_type, pss_options,
#endif
		&dgst->mbedtls_pk_context, mbedtls_md_get_type(md_info),
		hash_computed, sizeof(hash_computed),
		sigbuf, sigbuf_len);
	free(sigbuf);
	return ret;
}

static int mbedtls_rsa_dgst_init(struct swupdate_cfg *sw, const char *keyfile)
{
	struct mbedtls_digest *dgst;

	dgst = calloc(1, sizeof(*dgst));
	if (!dgst) {
		return -ENOMEM;
	}

#ifdef CONFIG_SIGNED_IMAGES
	if (swupdate_mbedtls_psa_init()) {
		ERROR("psa_crypto_init failed");
		free(dgst);
		return -EFAULT;
	}

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

__attribute__((constructor))
static void mbedtls_rsa_dgst(void)
{
	libs.dgst_init = mbedtls_rsa_dgst_init;
	libs.verify_file = mbedtls_rsa_verify_file;
#if defined(CONFIG_SIGALG_RAWRSA)
	(void)register_dgstlib(MODNAME, &libs);
#endif
#if defined(CONFIG_SIGALG_RSAPSS)
	(void)register_dgstlib(MODNAME_PSS, &libs);
#endif
}
