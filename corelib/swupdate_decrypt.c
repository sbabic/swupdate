/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * Code mostly taken from openssl examples
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "swupdate.h"
#include "sslapi.h"
#include "util.h"

struct swupdate_digest *swupdate_DECRYPT_init(unsigned char *key, unsigned char *iv)
{
	struct swupdate_digest *dec;
	int ret;

	if ((key == NULL) || (iv == NULL)) {
		ERROR("no key provided for decryption!");
		return NULL;
	}

	dec = calloc(1, sizeof(*dec));
	if (!dec) {
		return NULL;
	}

	EVP_CIPHER_CTX_init(&dec->ctxdec);

	/*
	 * Check openSSL documentation for return errors
	 */
	ret = EVP_DecryptInit_ex(&dec->ctxdec, EVP_aes_256_cbc(), NULL, key, iv);
	if (ret != 1) {
		ERROR("Decrypt Engine not initialized, error 0x%lx\n", ERR_get_error());
		free(dec);
		return NULL;
	}
#if 0
	if(!EVP_DecryptInit_ex(&dec->ctxdec, NULL, NULL, NULL, NULL)){
		ERROR("ERROR in EVP_DecryptInit_ex, 0x%lx\n", ERR_get_error());
		free(dec);
		return NULL;
	}
#endif

	return dec;
}

int swupdate_DECRYPT_update(struct swupdate_digest *dgst, unsigned char *buf, 
				int *outlen, unsigned char *cryptbuf, int inlen)
{
	if (EVP_DecryptUpdate(&dgst->ctxdec, buf, outlen, cryptbuf, inlen) != 1) {
		ERROR("Decryption error 0x%lx\n", ERR_get_error());
		return -EFAULT;
	}

	return 0;
}

int swupdate_DECRYPT_final(struct swupdate_digest *dgst, unsigned char *buf,
				int *outlen)
{
	if (!dgst)
		return -EINVAL;

	if (EVP_DecryptFinal_ex(&dgst->ctxdec, buf, outlen) != 1) {
		ERROR("Decryption error 0x%s\n", 
				ERR_reason_error_string(ERR_get_error()));
		return -EFAULT;
	}

	return 0;

}

void swupdate_DECRYPT_cleanup(struct swupdate_digest *dgst)
{
	if (dgst) {
		EVP_CIPHER_CTX_cleanup(&dgst->ctxdec);
		free(dgst);
		dgst = NULL;
	}
}
