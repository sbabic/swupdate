/*
 * (C) Copyright 2024
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include "util.h"

#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/cipher.h>
#include <mbedtls/version.h>

#define EVP_MAX_BLOCK_LENGTH (16)

struct swupdate_digest {
	mbedtls_md_context_t mbedtls_md_context;
	mbedtls_pk_context mbedtls_pk_context;
	mbedtls_cipher_context_t mbedtls_cipher_context;
};
