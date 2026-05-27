/*
 * (C) Copyright 2024
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include "util.h"

#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/cipher.h>
#include <mbedtls/version.h>
#include <mbedtls/oid.h>
#if defined(CONFIG_SIGALG_CMS) && MBEDTLS_VERSION_NUMBER >= 0x03040000
#include <mbedtls/pkcs7.h>
#endif
#if defined(MBEDTLS_USE_PSA_CRYPTO)
#include <psa/crypto.h>
#endif

struct mbedtls_digest {
	mbedtls_md_context_t mbedtls_md_context;
	mbedtls_pk_context mbedtls_pk_context;
#if defined(CONFIG_SIGALG_CMS) && MBEDTLS_VERSION_NUMBER >= 0x03040000
	mbedtls_x509_crt trusted_certs;
#endif
	mbedtls_cipher_context_t mbedtls_cipher_context;
	int cert_purpose;
};
