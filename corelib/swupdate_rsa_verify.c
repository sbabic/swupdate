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

#define BUFSIZE	(1024 * 8)

EVP_PKEY *load_pubkey(const char *file)
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

static int dgst_verify_init(struct swupdate_digest *dgst)
{
	int rc;

	rc = EVP_DigestVerifyInit(dgst->ctx, &dgst->ckey, EVP_sha256(), NULL, dgst->pkey);
	if (rc != 1) {
		ERROR("EVP_DigestVerifyInit failed, error 0x%lx", ERR_get_error());
		return -EFAULT; /* failed */
	}

#if defined(CONFIG_SIGALG_RSAPSS)
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
#endif

	return 0;
}

static int verify_update(struct swupdate_digest *dgst, char *msg, unsigned int mlen)
{
	int rc;

	rc = EVP_DigestVerifyUpdate(dgst->ctx, msg, mlen);
	if(rc != 1) {
		ERROR("EVP_DigestVerifyUpdate failed, error 0x%lx", ERR_get_error());
		return -EFAULT;
	}

	return 0;
}

static int verify_final(struct swupdate_digest *dgst, unsigned char *sig, unsigned int slen)
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

int swupdate_verify_file(struct swupdate_digest *dgst, const char *sigfile,
		const char *file, const char *signer_name)
{
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

	if ((dgst_init(dgst, EVP_sha256()) < 0) || (dgst_verify_init(dgst) < 0)) {
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



