/*
 * (C) Copyright 2019
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
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

#include "sslapi.h"
#include "util.h"

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
	return result;
}

int swupdate_verify_file(struct swupdate_digest *dgst, const char *sigfile,
		const char *file, const char *signer_name)
{
	int error;
	uint8_t hash_computed[32];
	const mbedtls_md_info_t *md_info;
	mbedtls_pk_type_t pk_type = MBEDTLS_PK_RSA;
	uint8_t signature[256];
	void *pss_options = NULL;
#if defined(CONFIG_SIGALG_RSAPSS)
	pk_type = MBEDTLS_PK_RSASSA_PSS;
	mbedtls_pk_rsassa_pss_options options = {
		.mgf1_hash_id = MBEDTLS_MD_SHA256,
		.expected_salt_len = MBEDTLS_RSA_SALT_LEN_ANY
	};
	pss_options = &options;
#endif

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
