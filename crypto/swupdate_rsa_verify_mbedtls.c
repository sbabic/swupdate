/*
 * (C) Copyright 2019
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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

static int read_file_into_buffer(uint8_t *buffer, int size, const char *filename)
{
	int fd;
	ssize_t rd;
	int result = -1;

	fd = open(filename, O_RDONLY);
	if (fd == -1) {
		ERROR("Failed to open file \"%s\"", filename);
		return -errno;
	}

	rd = read(fd, buffer, size);
	if (rd != size) {
		ERROR("Failed to read %d bytes from file \"%s\"", size, filename);
		result = -EMSGSIZE;
		goto exit;
	}

	result = 0;

exit:
	close(fd);
	return result;
}

static int mbedtls_rsa_verify_file(void *ctx, const char *sigfile,
		const char *file, const char *signer_name)
{
	struct mbedtls_digest *dgst = (struct mbedtls_digest *)ctx;
	int error;
	uint8_t hash_computed[32];
	const mbedtls_md_info_t *md_info;
	mbedtls_pk_type_t pk_type = MBEDTLS_PK_RSA;
	uint8_t signature[256];
	void *pss_options = NULL;
	mbedtls_pk_rsassa_pss_options options = {
		.mgf1_hash_id = MBEDTLS_MD_SHA256,
		.expected_salt_len = MBEDTLS_RSA_SALT_LEN_ANY
	};
	if (get_dgstlib() && !strcmp(get_dgstlib(), MODNAME_PSS)) {
		pk_type = MBEDTLS_PK_RSASSA_PSS;
		pss_options = &options;
	}

	(void)signer_name;

	md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	if (!md_info) {
		ERROR("mbedtls_md_info_from_type");
		return -ENOENT;
	}

	assert(mbedtls_md_get_size(md_info) == sizeof(hash_computed));

	error = mbedtls_md_file(md_info, file, hash_computed);
	if (error) {
		ERROR("mbedtls_md_file: %d", error);
		return error;
	}

	error = read_file_into_buffer(signature, sizeof(signature), sigfile);
	if (error) {
		return error;
	}

	return mbedtls_pk_verify_ext(
		pk_type, pss_options,
		&dgst->mbedtls_pk_context, mbedtls_md_get_type(md_info),
		hash_computed, sizeof(hash_computed),
		signature, sizeof(signature)
	);
}

static int mbedtls_rsa_dgst_init(struct swupdate_cfg *sw, const char *keyfile)
{
	struct mbedtls_digest *dgst;

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
