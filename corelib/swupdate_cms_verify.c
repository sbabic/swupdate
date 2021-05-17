/*
 * (C) Copyright 2019
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
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
#include "swupdate_verify_private.h"

#if defined(CONFIG_CMS_SKIP_UNKNOWN_SIGNERS)
#define VERIFY_UNKNOWN_SIGNER_FLAGS (CMS_NO_SIGNER_CERT_VERIFY)
#else
#define VERIFY_UNKNOWN_SIGNER_FLAGS (0)
#endif

int check_code_sign(const X509_PURPOSE *xp, const X509 *crt, int ca)
{
	X509 *x = (X509 *)crt;
	uint32_t ex_flags = SSL_X509_get_extension_flags(x);
	uint32_t ex_xkusage = SSL_X509_get_extended_key_usage(x);

	(void)xp;

	if (ca) {
		int idx;
		const X509_PURPOSE *pt;

		if ((ex_flags & EXFLAG_XKUSAGE) && !(ex_xkusage & XKU_CODE_SIGN))
			return 0;

		idx = X509_PURPOSE_get_by_id(X509_PURPOSE_OCSP_HELPER);
		if (idx == -1)
			return 0;

		pt = X509_PURPOSE_get0(idx);
		return pt->check_purpose(pt, x, ca);
	}

	return (ex_flags & EXFLAG_XKUSAGE) && (ex_xkusage & XKU_CODE_SIGN);
}

static int cms_verify_callback(int ok, X509_STORE_CTX *ctx) {
	int cert_error = X509_STORE_CTX_get_error(ctx);

	if (!ok) {
		switch (cert_error) {
#if defined(CONFIG_CMS_IGNORE_EXPIRED_CERTIFICATE)
		case X509_V_ERR_CERT_HAS_EXPIRED:
		case X509_V_ERR_CERT_NOT_YET_VALID:
			ok = 1;
			break;
#endif
#if defined(CONFIG_CMS_IGNORE_CERTIFICATE_PURPOSE)
		case X509_V_ERR_INVALID_PURPOSE:
			ok = 1;
			break;
#endif
		default:
			break;
		}
	}

	return ok;
}

X509_STORE *load_cert_chain(const char *file)
{
	X509_STORE *castore = X509_STORE_new();
	if (!castore) {
		return NULL;
	}

	/*
	 * Set error callback function for verification of CRTs and CRLs in order
	 * to ignore some errors depending on configuration
	 */
	X509_STORE_set_verify_cb(castore, cms_verify_callback);

	BIO *castore_bio = BIO_new_file(file, "r");
	if (!castore_bio) {
		TRACE("failed: BIO_new_file(%s)", file);
		return NULL;
	}

	int crt_count = 0;
	X509 *crt = NULL;
	do {
		crt = PEM_read_bio_X509(castore_bio, NULL, 0, NULL);
		if (crt) {
			crt_count++;
			char *subj = X509_NAME_oneline(X509_get_subject_name(crt), NULL, 0);
			char *issuer = X509_NAME_oneline(X509_get_issuer_name(crt), NULL, 0);
			TRACE("Read PEM #%d: %s %s", crt_count, issuer, subj);
			free(subj);
			free(issuer);
			if (X509_STORE_add_cert(castore, crt) == 0) {
				TRACE("Adding certificate to X509_STORE failed");
				BIO_free(castore_bio);
				X509_STORE_free(castore);
				return NULL;
			}
		}
	} while (crt);
	BIO_free(castore_bio);

	if (crt_count == 0) {
		X509_STORE_free(castore);
		return NULL;
	}

	return castore;
}

static inline int next_common_name(X509_NAME *subject, int i)
{
	return X509_NAME_get_index_by_NID(subject, NID_commonName, i);
}

static int check_common_name(X509_NAME *subject, const char *name)
{
	int i = -1, ret = 1;

	while ((i = next_common_name(subject, i)) > -1) {
		X509_NAME_ENTRY *e = X509_NAME_get_entry(subject, i);
		ASN1_STRING *d = X509_NAME_ENTRY_get_data(e);
		unsigned char *cn;
		size_t len = strlen(name);
		bool matches = (ASN1_STRING_to_UTF8(&cn, d) == (int)len)
				&& (strncmp(name, (const char *)cn, len) == 0);

		OPENSSL_free(cn);
		if (!matches) {
			char *subj = X509_NAME_oneline(subject, NULL, 0);

			ERROR("common name of '%s' does not match expected '%s'",
				subj, name);
			OPENSSL_free(subj);
			return 2;
		} else {
			ret = 0;
		}
	}

	if (ret == 0) {
		char *subj = X509_NAME_oneline(subject, NULL, 0);

		TRACE("verified signer cert: %s", subj);
		OPENSSL_free(subj);
	}

	return ret;
}

static int check_signer_name(CMS_ContentInfo *cms, const char *name)
{
	STACK_OF(CMS_SignerInfo) *infos = CMS_get0_SignerInfos(cms);
	STACK_OF(X509) *crts;
	int i, ret = 1;

	if ((name == NULL) || (name[0] == '\0'))
		return 0;

	crts = CMS_get1_certs(cms);
	for (i = 0; i < sk_CMS_SignerInfo_num(infos); ++i) {
		CMS_SignerInfo *si = sk_CMS_SignerInfo_value(infos, i);
		int j;

		for (j = 0; j < sk_X509_num(crts); ++j) {
			X509 *crt = sk_X509_value(crts, j);

			if (CMS_SignerInfo_cert_cmp(si, crt) == 0) {
				ret = check_common_name(
					X509_get_subject_name(crt), name);
			}
		}
	}
	sk_X509_pop_free(crts, X509_free);

	return ret;
}

#if defined(CONFIG_CMS_SKIP_UNKNOWN_SIGNERS)
static int check_verified_signer(CMS_ContentInfo* cms, X509_STORE* store)
{
	int i, ret = 1;

	X509_STORE_CTX *ctx = X509_STORE_CTX_new();
	STACK_OF(CMS_SignerInfo) *infos = CMS_get0_SignerInfos(cms);
	STACK_OF(X509)* cms_certs = CMS_get1_certs(cms);

	if (!ctx) {
		ERROR("Failed to allocate verification context");
		return ret;
	}

	for (i = 0; i < sk_CMS_SignerInfo_num(infos) && ret != 0; ++i) {
		CMS_SignerInfo *si = sk_CMS_SignerInfo_value(infos, i);
		X509 *signer = NULL;

		CMS_SignerInfo_get0_algs(si, NULL, &signer, NULL, NULL);
		if (!X509_STORE_CTX_init(ctx, store, signer, cms_certs)) {
			ERROR("Failed to initialize signer verification operation");
			break;
		}

		X509_STORE_CTX_set_default(ctx, "smime_sign");
		if (X509_verify_cert(ctx) > 0) {
			TRACE("Verified signature %d in signer sequence", i);
			ret = 0;
		} else {
			TRACE("Failed to verify certificate %d in signer sequence", i);
		}

		X509_STORE_CTX_cleanup(ctx);
	}

	X509_STORE_CTX_free(ctx);

	return ret;
}
#endif

int swupdate_verify_file(struct swupdate_digest *dgst, const char *sigfile,
		const char *file, const char *signer_name)
{
	int status = -EFAULT;
	CMS_ContentInfo *cms = NULL;
	BIO *content_bio = NULL;

	/* Open CMS blob that needs to be checked */
	BIO *sigfile_bio = BIO_new_file(sigfile, "rb");
	if (!sigfile_bio) {
		ERROR("%s cannot be opened", sigfile);
		status = -EBADF;
		goto out;
	}

	/* Parse the DER-encoded CMS message */
	cms = d2i_CMS_bio(sigfile_bio, NULL);
	if (!cms) {
		ERROR("%s cannot be parsed as DER-encoded CMS signature blob", sigfile);
		status = -EFAULT;
		goto out;
	}

	if (check_signer_name(cms, signer_name)) {
		ERROR("failed to verify signer name");
		status = -EFAULT;
		goto out;
	}

	/* Open the content file (data which was signed) */
	content_bio = BIO_new_file(file, "rb");
	if (!content_bio) {
		ERROR("%s cannot be opened", file);
		status = -EBADF;
		goto out;
	}

	/* Then try to verify signature */
	if (!CMS_verify(cms, NULL, dgst->certs, content_bio,
			NULL, CMS_BINARY | VERIFY_UNKNOWN_SIGNER_FLAGS)) {
		ERR_print_errors_fp(stderr);
		ERROR("Signature verification failed");
		status = -EBADMSG;
		goto out;
	}

#if defined(CONFIG_CMS_SKIP_UNKNOWN_SIGNERS)
	/* Verify at least one signer authenticates */
	if (check_verified_signer(cms, dgst->certs)) {
		ERROR("Authentication of all signatures failed");
		status = -EBADMSG;
		goto out;
	}
#endif

	TRACE("Verified OK");

	/* Signature is valid */
	status = 0;
out:

	if (cms) {
		CMS_ContentInfo_free(cms);
	}
	if (content_bio) {
		BIO_free(content_bio);
	}
	if (sigfile_bio) {
		BIO_free(sigfile_bio);
	}
	return status;
}
