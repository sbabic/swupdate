// SPDX-FileCopyrightText: 2019 Laszlo Ashin <laszlo@ashin.hu>
//
// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>

#include "sslapi.h"
#include "util.h"

struct swupdate_digest *swupdate_DECRYPT_init(unsigned char *key, char keylen, unsigned char *iv)
{
	struct swupdate_digest *dgst;
	mbedtls_cipher_type_t cipher_type;
	const mbedtls_cipher_info_t *cipher_info;
	int key_bitlen;
	int error;

	if ((key == NULL) || (iv == NULL)) {
		ERROR("no key provided for decryption!");
		return NULL;
	}

	switch (keylen) {
	case AES_128_KEY_LEN:
		cipher_type = MBEDTLS_CIPHER_AES_128_CBC;
		key_bitlen = 128;
		break;
	case AES_192_KEY_LEN:
		cipher_type = MBEDTLS_CIPHER_AES_192_CBC;
		key_bitlen = 192;
		break;
	case AES_256_KEY_LEN:
		cipher_type = MBEDTLS_CIPHER_AES_256_CBC;
		key_bitlen = 256;
		break;
	default:
		return NULL;
	}

	cipher_info = mbedtls_cipher_info_from_type(cipher_type);
	if (!cipher_info) {
		ERROR("mbedtls_cipher_info_from_type");
		return NULL;
	}

	dgst = calloc(1, sizeof(*dgst));
	if (!dgst) {
		return NULL;
	}

	mbedtls_cipher_init(&dgst->mbedtls_cipher_context);

	error = mbedtls_cipher_setup(&dgst->mbedtls_cipher_context, cipher_info);
	if (error) {
		ERROR("mbedtls_cipher_setup: %d", error);
		goto fail;
	}

	error = mbedtls_cipher_setkey(&dgst->mbedtls_cipher_context, key, key_bitlen, MBEDTLS_DECRYPT);
	if (error) {
		ERROR("mbedtls_cipher_setkey: %d", error);
		goto fail;
	}

	error = mbedtls_cipher_set_iv(&dgst->mbedtls_cipher_context, iv, 16);
	if (error) {
		ERROR("mbedtls_cipher_set_iv: %d", error);
		goto fail;
	}

	return dgst;

fail:
	free(dgst);
	return NULL;
}

int swupdate_DECRYPT_update(struct swupdate_digest *dgst, unsigned char *buf,
				int *outlen, const unsigned char *cryptbuf, int inlen)
{
	int error;
	size_t olen = *outlen;

	error = mbedtls_cipher_update(&dgst->mbedtls_cipher_context, cryptbuf, inlen, buf, &olen);
	if (error) {
		ERROR("mbedtls_cipher_update: %d", error);
		return -EFAULT;
	}
	*outlen = olen;

	return 0;
}

int swupdate_DECRYPT_final(struct swupdate_digest *dgst, unsigned char *buf,
				int *outlen)
{
	int error;
	size_t olen = *outlen;

	if (!dgst) {
		return -EINVAL;
	}

	error = mbedtls_cipher_finish(&dgst->mbedtls_cipher_context, buf, &olen);
	if (error) {
		ERROR("mbedtls_cipher_finish: %d", error);
		return -EFAULT;
	}
	*outlen = olen;

	return 0;

}

void swupdate_DECRYPT_cleanup(struct swupdate_digest *dgst)
{
	if (!dgst) {
		return;
	}

	mbedtls_cipher_free(&dgst->mbedtls_cipher_context);
	free(dgst);
}
