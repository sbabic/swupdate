/*
 * (C) Copyright 2024
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include "util.h"

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/openssl/bio.h>
#include <wolfssl/openssl/objects.h>
#include <wolfssl/openssl/err.h>
#include <wolfssl/openssl/x509.h>
#include <wolfssl/openssl/x509v3.h>
#include <wolfssl/openssl/pem.h>
#include <wolfssl/openssl/evp.h>
#include <wolfssl/openssl/hmac.h>
#include <wolfssl/openssl/aes.h>
#include <wolfssl/openssl/opensslv.h>
#include <wolfssl/openssl/pkcs7.h>

#define SSL_GET_CTXDEC(dgst) dgst->ctxdec

#define X509_PURPOSE_CODE_SIGN EXTKEYUSE_CODESIGN
#define SSL_PURPOSE_EMAIL_PROT EXTKEYUSE_EMAILPROT

#define openssl_digest wolfssl_digest

struct wolfssl_digest {
	EVP_PKEY *pkey;		/* this is used for RSA key */
	EVP_PKEY_CTX *ckey;	/* this is used for RSA key */
	X509_STORE *certs;	/* this is used if CMS is set */
	EVP_MD_CTX *ctx;
	EVP_CIPHER_CTX *ctxdec;
};
