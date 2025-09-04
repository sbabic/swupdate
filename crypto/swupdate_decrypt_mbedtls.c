// SPDX-FileCopyrightText: 2019 Laszlo Ashin <laszlo@ashin.hu>
//
// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>

#include "util.h"
#include "swupdate_crypto.h"
#include "swupdate_mbedtls.h"

#define MODNAME	"mbedtlsAES"

static swupdate_decrypt_lib mbedtls;

static void *mbedtls_DECRYPT_init(unsigned char *key, char keylen, unsigned char *iv)
{
	struct mbedtls_digest *dgst;
	mbedtls_cipher_type_t cipher_type;
	const mbedtls_cipher_info_t *cipher_info;
	int key_bitlen;
	int error;

	if ((key == NULL) || (iv == NULL)) {
		ERROR("no key or iv provided for decryption!");
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

#ifdef MBEDTLS_CIPHER_MODE_WITH_PADDING
	error = mbedtls_cipher_set_padding_mode(&dgst->mbedtls_cipher_context, MBEDTLS_PADDING_PKCS7);
	if (error) {
		ERROR("mbedtls_cipher_set_padding_mode: %d", error);
		goto fail;
	}
#endif

	error = mbedtls_cipher_set_iv(&dgst->mbedtls_cipher_context, iv, 16);
	if (error) {
		ERROR("mbedtls_cipher_set_iv: %d", error);
		goto fail;
	}

	error = mbedtls_cipher_reset(&dgst->mbedtls_cipher_context);
	if (error) {
		ERROR("mbedtls_cipher_reset: %d", error);
		goto fail;
	}

	return dgst;

fail:
	free(dgst);
	return NULL;
}

static int mbedtls_DECRYPT_update(void *ctx, unsigned char *buf,
				int *outlen, const unsigned char *cryptbuf, int inlen)
{
	struct mbedtls_digest *dgst = (struct mbedtls_digest *)ctx;
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

static int mbedtls_DECRYPT_final(void *ctx, unsigned char *buf,
				int *outlen)
{
	int error;
	size_t olen = *outlen;
	struct mbedtls_digest *dgst = (struct mbedtls_digest *)ctx;

	if (!dgst) {
		return -EINVAL;
	}

	error = mbedtls_cipher_finish(&dgst->mbedtls_cipher_context, buf, &olen);
	if (error) {
#ifndef CONFIG_ENCRYPTED_IMAGES_HARDEN_LOGGING
		ERROR("mbedtls_cipher_finish: %d", error);
#endif
		return -EFAULT;
	}
	*outlen = olen;

	return 0;

}

static void mbedtls_DECRYPT_cleanup(void *ctx)
{
	struct mbedtls_digest *dgst = (struct mbedtls_digest *)ctx;

	if (!dgst) {
		return;
	}

	mbedtls_cipher_free(&dgst->mbedtls_cipher_context);
	free(dgst);
}

__attribute__((constructor))
static void mbedtls_probe(void)
{
	mbedtls.DECRYPT_init = mbedtls_DECRYPT_init;
	mbedtls.DECRYPT_update = mbedtls_DECRYPT_update;
	mbedtls.DECRYPT_final = mbedtls_DECRYPT_final;
	mbedtls.DECRYPT_cleanup = mbedtls_DECRYPT_cleanup;
	(void)register_cryptolib(MODNAME, &mbedtls);
}
