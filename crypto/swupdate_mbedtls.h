/*
 * (C) Copyright 2024
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <errno.h>
#include <stdint.h>
#include "util.h"

#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/version.h>
#include <mbedtls/oid.h>
#if MBEDTLS_VERSION_NUMBER < 0x04000000
#include <mbedtls/cipher.h>
#endif
#if defined(CONFIG_SIGALG_CMS) && MBEDTLS_VERSION_NUMBER >= 0x03040000
#include <mbedtls/pkcs7.h>
#endif
#if MBEDTLS_VERSION_NUMBER >= 0x04000000 || defined(MBEDTLS_USE_PSA_CRYPTO)
#include <psa/crypto.h>
#endif

struct mbedtls_digest {
	mbedtls_md_context_t mbedtls_md_context;
	const mbedtls_md_info_t *mbedtls_md_info;
	mbedtls_pk_context mbedtls_pk_context;
#if defined(CONFIG_SIGALG_CMS) && MBEDTLS_VERSION_NUMBER >= 0x03040000
	mbedtls_x509_crt trusted_certs;
#endif
#if MBEDTLS_VERSION_NUMBER >= 0x04000000
	psa_cipher_operation_t psa_cipher_operation;
	psa_key_id_t psa_key_id;
#else
	mbedtls_cipher_context_t mbedtls_cipher_context;
#endif
	int cert_purpose;
};

/*
 * With mbedTLS 4.x, PSA is the only crypto API: it must be
 * initialized before any hash, cipher, PK or X.509 operation.
 * psa_crypto_init() can be called more than once.
 */
static inline int swupdate_mbedtls_psa_init(void)
{
#if MBEDTLS_VERSION_NUMBER >= 0x04000000 || defined(MBEDTLS_USE_PSA_CRYPTO)
	if (psa_crypto_init() != PSA_SUCCESS)
		return -EFAULT;
#endif
	return 0;
}
