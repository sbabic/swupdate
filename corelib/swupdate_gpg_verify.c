/*
 * Author: Amy Fong
 * Copyright (C) 2023, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "swupdate.h"
#include "sslapi.h"
#include "util.h"

#include <errno.h>
#include <locale.h>
#include <gpgme.h>

static gpg_error_t
status_cb(void *opaque, const char *keyword, const char *value)
{
	(void)opaque;
	DEBUG("status_cb: %s %s", keyword, value);
	return 0;
}

#define MSGBUF_LEN 256

int swupdate_verify_file(struct swupdate_digest *dgst, const char *sigfile,
                const char *file, const char *signer_name)
{
	gpgme_ctx_t ctx;
	gpgme_error_t err;
	gpgme_data_t image_sig, image;
	FILE *fp_sig = NULL;
	FILE *fp = NULL;
	gpgme_signature_t sig;
	int status = 0;
	gpgme_protocol_t protocol;
	gpgme_verify_result_t result;
	char msg[MSGBUF_LEN];
	int r;
	const char *tmp;

	tmp = gpgme_check_version(NULL);
	if (tmp == NULL) {
		ERROR("Failed to check gpgme library version");
		status = -EFAULT;
		goto out;
	}

	err = gpgme_new(&ctx);
	if (err) {
		ERROR("Failed to create new gpg context");
		r = gpgme_strerror_r(err, msg, MSGBUF_LEN);
		if (r == 0) {
			ERROR("Reason: %s", msg);
		}
		status = -EFAULT;
		goto out;
	}

	if (dgst->gpgme_protocol != NULL) {
		DEBUG("gpg: Enabling protocol %s", dgst->gpgme_protocol);
		if (!strcmp(dgst->gpgme_protocol, "openpgp")) {
			TRACE("gpg: using protocol OpenPGP");
			protocol = GPGME_PROTOCOL_OpenPGP;
		} else if (!strcmp(dgst->gpgme_protocol, "cms")) {
			TRACE("gpg: using protocol cms");
			protocol = GPGME_PROTOCOL_CMS;
		} else {
			ERROR("gpg: unsupported protocol! %s", dgst->gpgme_protocol);
			status = -EFAULT;
			goto out;
		}
	} else {
		ERROR("gpg protocol unspecified!");
		status = -EFAULT;
		goto out;
	}

	gpgme_set_protocol(ctx, protocol);
	gpgme_set_status_cb(ctx, status_cb, NULL);
	if (dgst->verbose == 1) {
		gpgme_set_ctx_flag(ctx, "full-status", "1");
	}
	gpgme_set_locale(ctx, LC_ALL, setlocale(LC_ALL, ""));

	if (dgst->gpg_home_directory != NULL) {
		err = gpgme_ctx_set_engine_info(ctx, protocol, NULL, dgst->gpg_home_directory);
		if (err) {
			ERROR("Something went wrong while setting the engine info");
			r = gpgme_strerror_r(err, msg, MSGBUF_LEN);
			if (r == 0) {
				ERROR("Reason: %s", msg);
			}
			status = -EFAULT;
			goto out;
		}
	}

	fp_sig = fopen(sigfile, "rb");
	if (!fp_sig) {
		ERROR("Failed to open %s", sigfile);
		status = -EBADF;
		goto out;
	}
	err = gpgme_data_new_from_stream(&image_sig, fp_sig);
	if (err) {
		ERROR("error allocating data object");
		r = gpgme_strerror_r(err, msg, MSGBUF_LEN);
		if (r == 0) {
			ERROR("Reason: %s", msg);
		}
		status = -ENOMEM;
		goto out;
	}

	fp = fopen(file, "rb");
	if (!fp) {
		ERROR("Failed to open %s", file);
		status = -EBADF;
		goto out;
	}
	err = gpgme_data_new_from_stream(&image, fp);
	if (err) {
		ERROR("error allocating data object");
		r = gpgme_strerror_r(err, msg, MSGBUF_LEN);
		if (r == 0) {
			ERROR("Reason: %s", msg);
		}
		status = -ENOMEM;
		goto out;
	}

	err = gpgme_op_verify(ctx, image_sig, image, NULL);
	result = gpgme_op_verify_result(ctx);
	if (err) {
		ERROR("verify failed");
		r = gpgme_strerror_r(err, msg, MSGBUF_LEN);
		if (r == 0) {
			ERROR("Reason: %s", msg);
		}
		status = -EBADMSG;
		goto out;
	}

	if (result) {
		for (sig = result->signatures; sig; sig = sig->next) {
			if (sig->status == GPG_ERR_NO_ERROR) {
				TRACE("Verified OK");
				status = 0;
				goto out;
			}
		}
	}
	TRACE("Verification failed");
	status = -EBADMSG;

 out:
	gpgme_data_release(image);
	gpgme_data_release(image_sig);
	gpgme_release(ctx);

	if (fp)
		fclose(fp);
	if (fp_sig)
		fclose(fp_sig);

	return status;
}
