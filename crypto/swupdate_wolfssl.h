/*
 * (C) Copyright 2024
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include "util.h"

#ifdef CONFIG_PKCS11
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/wc_pkcs11.h>
// Exclude p11-kit's pkcs11.h to prevent conflicting with wolfssl's
#define PKCS11_H 1
#include <p11-kit/uri.h>
#endif

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

#define EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx, len) (1)

#define X509_PURPOSE_CODE_SIGN EXTKEYUSE_CODESIGN
#define SSL_PURPOSE_EMAIL_PROT EXTKEYUSE_EMAILPROT

#define openssl_digest wolfssl_digest

struct wolfssl_digest {
	EVP_PKEY *pkey;		/* this is used for RSA key */
	EVP_PKEY_CTX *ckey;	/* this is used for RSA key */
	X509_STORE *certs;	/* this is used if CMS is set */
	EVP_MD_CTX *ctx;
#ifdef CONFIG_PKCS11
	unsigned char last_decr[AES_BLOCK_SIZE + 1];
	P11KitUri *p11uri;
	Aes ctxdec;
	Pkcs11Dev pkdev;
	Pkcs11Token pktoken;
#endif
};
