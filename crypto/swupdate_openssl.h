/*
 * (C) Copyright 2016-2024
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include "util.h"

#include <openssl/bio.h>
#include <openssl/objects.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/aes.h>
#include <openssl/opensslv.h>
#include <openssl/cms.h>

#if !defined(X509_PURPOSE_CODE_SIGN)
#define X509_PURPOSE_CODE_SIGN (X509_PURPOSE_MAX + 1)
#endif

#define SSL_PURPOSE_EMAIL_PROT X509_PURPOSE_SMIME_SIGN

#define SSL_PURPOSE_CODE_SIGN  X509_PURPOSE_CODE_SIGN
#define SSL_PURPOSE_DEFAULT SSL_PURPOSE_EMAIL_PROT

struct swupdate_digest {
	EVP_PKEY *pkey;		/* this is used for RSA key */
	EVP_PKEY_CTX *ckey;	/* this is used for RSA key */
	X509_STORE *certs;	/* this is used if CMS is set */
	EVP_MD_CTX *ctx;
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	EVP_CIPHER_CTX ctxdec;
#else
	EVP_CIPHER_CTX *ctxdec;
#endif
};

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define SSL_GET_CTXDEC(dgst) &dgst->ctxdec
#else
#define SSL_GET_CTXDEC(dgst) dgst->ctxdec
#endif


