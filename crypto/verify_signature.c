/*
 * (C) Copyright 2016
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 *
 * Code mostly taken from openssl examples
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "swupdate.h"
#include "sslapi.h"
#include "util.h"
#include "compat.h"
#include "swupdate_verify_private.h"

int swupdate_dgst_init(struct swupdate_cfg *sw, const char *keyfile)
{
	struct swupdate_digest *dgst;
	int ret;

	/*
	 * Check that it was not called before
	 */
	if (sw->dgst) {
		return -EBUSY;
	}

	dgst = calloc(1, sizeof(*dgst));
	if (!dgst) {
		ret = -ENOMEM;
		goto dgst_init_error;
	}

#if defined(CONFIG_SIGALG_RAWRSA) || defined(CONFIG_SIGALG_RSAPSS)
	/*
	 * Load public key
	 */
	dgst->pkey = load_pubkey(keyfile);
	if (!dgst->pkey) {
		ERROR("Error loading pub key from %s", keyfile);
		ret = -EINVAL;
		goto dgst_init_error;
	}
	dgst->ckey = EVP_PKEY_CTX_new(dgst->pkey, NULL);
	if (!dgst->ckey) {
		ERROR("Error creating context key for %s", keyfile);
		ret = -EINVAL;
		goto dgst_init_error;
	}
#elif defined(CONFIG_SIGALG_CMS)
	/*
	 * Load certificate chain
	 */
	dgst->certs = load_cert_chain(keyfile);
	if (!dgst->certs) {
		ERROR("Error loading certificate chain from %s", keyfile);
		ret = -EINVAL;
		goto dgst_init_error;
	}

#ifndef CONFIG_CMS_IGNORE_CERTIFICATE_PURPOSE
	{
		static char code_sign_name[] = "Code signing";
		static char code_sign_sname[] = "codesign";

		if (!X509_PURPOSE_add(X509_PURPOSE_CODE_SIGN, X509_TRUST_EMAIL,
				0, check_code_sign, code_sign_name,
				code_sign_sname, NULL)) {
			ERROR("failed to add code sign purpose");
			ret = -EINVAL;
			goto dgst_init_error;
		}
	}

	if (!X509_STORE_set_purpose(dgst->certs, sw->cert_purpose)) {
		ERROR("failed to set purpose");
		ret = -EINVAL;
		goto dgst_init_error;
	}
#endif

#elif defined(CONFIG_SIGALG_GPG)
	dgst->gpg_home_directory = sw->gpg_home_directory;
	dgst->gpgme_protocol = sw->gpgme_protocol;
	dgst->verbose = sw->verbose;
#else
	TRACE("public key / cert %s ignored, you need to set SIGALG", keyfile);
#endif

	/*
	 * Create context
	 */
	dgst->ctx = EVP_MD_CTX_create();
	if(dgst->ctx == NULL) {
		ERROR("EVP_MD_CTX_create failed, error 0x%lx", ERR_get_error());
		ret = -ENOMEM;
		goto dgst_init_error;
	}

	sw->dgst = dgst;

	return 0;

dgst_init_error:
	if (dgst)
		free(dgst);

	return ret;
}
