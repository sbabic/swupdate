// SPDX-FileCopyrightText: 2019 Laszlo Ashin <laszlo@ashin.hu>
//
// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>

#include "util.h"
#include "swupdate_crypto.h"
#include "swupdate_mbedtls.h"

#define MODNAME	"mbedtlsAES"

static swupdate_decrypt_lib mbedtls;

#if MBEDTLS_VERSION_NUMBER >= 0x04000000
static void *mbedtls_DECRYPT_init(unsigned char *key, char keylen, unsigned char *iv, cipher_t cipher)
{
	struct mbedtls_digest *dgst;
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_status_t status;

	/* only AES-CBC is supported */
	(void)cipher;

	if ((key == NULL) || (iv == NULL)) {
		ERROR("no key or iv provided for decryption!");
		return NULL;
	}

	switch (keylen) {
	case AES_128_KEY_LEN:
	case AES_192_KEY_LEN:
	case AES_256_KEY_LEN:
		break;
	default:
		return NULL;
	}

	if (swupdate_mbedtls_psa_init()) {
		ERROR("psa_crypto_init failed");
		return NULL;
	}

	dgst = calloc(1, sizeof(*dgst));
	if (!dgst) {
		return NULL;
	}

	dgst->psa_cipher_operation = psa_cipher_operation_init();
	dgst->psa_key_id = PSA_KEY_ID_NULL;

	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attributes, PSA_ALG_CBC_PKCS7);
	psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attributes, keylen * 8);

	status = psa_import_key(&attributes, key, keylen, &dgst->psa_key_id);
	psa_reset_key_attributes(&attributes);
	if (status != PSA_SUCCESS) {
		ERROR("psa_import_key: %d", (int)status);
		goto fail;
	}

	status = psa_cipher_decrypt_setup(&dgst->psa_cipher_operation,
			dgst->psa_key_id, PSA_ALG_CBC_PKCS7);
	if (status != PSA_SUCCESS) {
		ERROR("psa_cipher_decrypt_setup: %d", (int)status);
		goto fail;
	}

	status = psa_cipher_set_iv(&dgst->psa_cipher_operation, iv, AES_BLK_SIZE);
	if (status != PSA_SUCCESS) {
		ERROR("psa_cipher_set_iv: %d", (int)status);
		goto fail;
	}

	return dgst;

fail:
	psa_cipher_abort(&dgst->psa_cipher_operation);
	psa_destroy_key(dgst->psa_key_id);
	free(dgst);
	return NULL;
}

/*
 * As with the other backends, buf can hold inlen + AES_BLK_SIZE bytes
 */
static int mbedtls_DECRYPT_update(void *ctx, unsigned char *buf,
				int *outlen, const unsigned char *cryptbuf, int inlen)
{
	struct mbedtls_digest *dgst = (struct mbedtls_digest *)ctx;
	psa_status_t status;
	size_t olen = 0;

	if (!dgst || inlen < 0) {
		return -EINVAL;
	}

	status = psa_cipher_update(&dgst->psa_cipher_operation, cryptbuf, inlen,
			buf, (size_t)inlen + AES_BLK_SIZE, &olen);
	if (status != PSA_SUCCESS) {
		ERROR("psa_cipher_update: %d", (int)status);
		return -EFAULT;
	}
	*outlen = olen;

	return 0;
}

static int mbedtls_DECRYPT_final(void *ctx, unsigned char *buf,
				int *outlen)
{
	struct mbedtls_digest *dgst = (struct mbedtls_digest *)ctx;
	psa_status_t status;
	size_t olen = 0;

	if (!dgst) {
		return -EINVAL;
	}

	status = psa_cipher_finish(&dgst->psa_cipher_operation, buf,
			AES_BLK_SIZE, &olen);
	if (status != PSA_SUCCESS) {
#ifndef CONFIG_ENCRYPTED_IMAGES_HARDEN_LOGGING
		ERROR("psa_cipher_finish: %d", (int)status);
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

	psa_cipher_abort(&dgst->psa_cipher_operation);
	psa_destroy_key(dgst->psa_key_id);
	free(dgst);
}
#else
static void *mbedtls_DECRYPT_init(unsigned char *key, char keylen, unsigned char *iv, cipher_t cipher)
{
	struct mbedtls_digest *dgst;
	mbedtls_cipher_type_t cipher_type;
	const mbedtls_cipher_info_t *cipher_info;
	int key_bitlen;
	int error;

	/* Temporary to remove warning */
	cipher = cipher;

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

#endif

__attribute__((constructor))
static void mbedtls_probe(void)
{
	mbedtls.DECRYPT_init = mbedtls_DECRYPT_init;
	mbedtls.DECRYPT_update = mbedtls_DECRYPT_update;
	mbedtls.DECRYPT_final = mbedtls_DECRYPT_final;
	mbedtls.DECRYPT_cleanup = mbedtls_DECRYPT_cleanup;
	(void)register_cryptolib(MODNAME, &mbedtls);
}
