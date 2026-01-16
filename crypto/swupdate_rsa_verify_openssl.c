/*
 * (C) Copyright 2019
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
#include "util.h"
#include "swupdate_crypto.h"
#include "swupdate_openssl.h"

#define BUFSIZE	(1024 * 8)

#define MODNAME	"opensslRSA"
#define MODNAME_PSS	"opensslRSAPSS"

static swupdate_dgst_lib	libs;

static EVP_PKEY *load_pubkey(const char *file)
{
	BIO *key=NULL;
	EVP_PKEY *pkey=NULL;

	if (file == NULL)
	{
		ERROR("no keyfile specified");
		goto end;
	}

	key=BIO_new(BIO_s_file());
	if (key == NULL)
	{
		goto end;
	}

	if (BIO_read_filename(key, file) <= 0)
	{
		printf("Error opening %s \n", file);
		goto end;
	}

	pkey=PEM_read_bio_PUBKEY(key, NULL, NULL, NULL);
end:
	if (key != NULL) BIO_free(key);
	if (pkey == NULL)
		ERROR("unable to load key filename %s", file);
	return(pkey);
}

static int dgst_verify_init(struct openssl_digest *dgst)
{
	int rc;

	rc = EVP_DigestVerifyInit(dgst->ctx, &dgst->ckey, EVP_sha256(), NULL, dgst->pkey);
	if (rc != 1) {
		ERROR("EVP_DigestVerifyInit failed, error 0x%lx", ERR_get_error());
		return -EFAULT; /* failed */
	}

	if (get_dgstlib() && !strcmp(get_dgstlib(), MODNAME_PSS)) {
		rc = EVP_PKEY_CTX_set_rsa_padding(dgst->ckey, RSA_PKCS1_PSS_PADDING);
		if (rc <= 0) {
			ERROR("EVP_PKEY_CTX_set_rsa_padding failed, error 0x%lx", ERR_get_error());
			return -EFAULT; /* failed */
		}
		rc = EVP_PKEY_CTX_set_rsa_pss_saltlen(dgst->ckey, -2);
		if (rc <= 0) {
			ERROR("EVP_PKEY_CTX_set_rsa_pss_saltlen failed, error 0x%lx", ERR_get_error());
			return -EFAULT; /* failed */
		}
	}

	return 0;
}

static int verify_update(struct openssl_digest *dgst, char *msg, unsigned int mlen)
{
	int rc;

	rc = EVP_DigestVerifyUpdate(dgst->ctx, msg, mlen);
	if(rc != 1) {
		ERROR("EVP_DigestVerifyUpdate failed, error 0x%lx", ERR_get_error());
		return -EFAULT;
	}

	return 0;
}

static int verify_final(struct openssl_digest *dgst, unsigned char *sig, unsigned int slen)
{
	unsigned int rc;

	/* Clear any errors for the call below */
	ERR_clear_error();
	rc = EVP_DigestVerifyFinal(dgst->ctx, sig, slen);
	if(rc != 1) {
		ERROR("EVP_DigestVerifyFinal failed, error 0x%lx %d", ERR_get_error(), rc);
		return -1;
	}

	return rc;
}

static int openssl_rsa_verify_file(void *ctx, const char *sigfile,
		const char *file, const char *signer_name)
{
	struct openssl_digest *dgst = (struct openssl_digest *)ctx;
	FILE *fp = NULL;
	BIO *sigbio;
	int siglen = 0;
	int i;
	unsigned char *sigbuf = NULL;
	char *msg = NULL;
	int size;
	size_t rbytes;
	int status = 0;

	(void)signer_name;
	if (!dgst) {
		ERROR("Wrong crypto initialization: did you pass the key ?");
		status = -ENOKEY;
		goto out;
	}

	msg = malloc(BUFSIZE);
	if (!msg) {
		status = -ENOMEM;
		goto out;
	}

	sigbio = BIO_new_file(sigfile, "rb");
	siglen = EVP_PKEY_size(dgst->pkey);
	sigbuf = OPENSSL_malloc(siglen);

	siglen = BIO_read(sigbio, sigbuf, siglen);
	BIO_free(sigbio);

	if(siglen <= 0) {
		ERROR("Error reading signature file %s", sigfile);
		status = -ENOKEY;
		goto out;
	}

	ERR_clear_error();
	if (EVP_DigestInit_ex(dgst->ctx, EVP_sha256(), NULL) != 1) {
		ERROR("EVP_DigestInit_ex failed: %s", ERR_error_string(ERR_get_error(), NULL));
		status = -ENOKEY;
		goto out;
	}

	if (dgst_verify_init(dgst) < 0) {
		status = -ENOKEY;
		goto out;
	}

	fp = fopen(file, "r");
	if (!fp) {
		ERROR("%s cannot be opened", file);
		status = -EBADF;
		goto out;
	}

	size = 0;
	for (;;) {
		rbytes = fread(msg, 1, BUFSIZE, fp);
		if (rbytes > 0) {
			size += rbytes;
			if (verify_update(dgst, msg, rbytes) < 0)
				break;
		}
		if (feof(fp))
			break;
	}

	TRACE("Verify signed image: Read %d bytes", size);
	i = verify_final(dgst, sigbuf, (unsigned int)siglen);
	if(i > 0) {
		TRACE("Verified OK");
		status = 0;
	} else if(i == 0) {
		TRACE("Verification Failure");
		status = -EBADMSG;
	} else {
		TRACE("Error Verifying Data");
		status = -EFAULT;
	}

out:
	if (fp)
		fclose(fp);
	if (msg)
		free(msg);
	if (sigbuf)
		OPENSSL_free(sigbuf);

	return status;
}

static int openssl_rsa_dgst_init(struct swupdate_cfg *sw, const char *keyfile)
{
	struct openssl_digest *dgst;
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

__attribute__((constructor))
static void openssl_dgst(void)
{
	libs.dgst_init = openssl_rsa_dgst_init;
	libs.verify_file = openssl_rsa_verify_file;
#if defined(CONFIG_SIGALG_RAWRSA)
	(void)register_dgstlib(MODNAME, &libs);
#endif
#if defined(CONFIG_SIGALG_RSAPSS)
	(void)register_dgstlib(MODNAME_PSS, &libs);
#endif
}
