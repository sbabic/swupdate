/*
 * (C) Copyright 2020, Linutronix GmbH
 * Author: Bastian Germann
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "swupdate.h"
#include "swupdate_wolfssl.h"
#include "util.h"
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/logging.h>
#include "swupdate_crypto.h"

static swupdate_decrypt_lib wolfssl;

#ifdef DEBUG_WOLFSSL
static void wolfssl_debug(int __attribute__ ((__unused__)) level, const char *const msg)
{
	DEBUG("%s", msg);
}
#endif

static void *wolfssl_DECRYPT_init(unsigned char *key,
					char __attribute__ ((__unused__)) keylen, unsigned char *iv,
					cipher_t cipher)
{
	struct wolfssl_digest *dgst;
	const char *library;
	const char *pin;
	const char *msg;
	CK_ATTRIBUTE_PTR key_id;
	int slot_id;
	int err = 0;
	int dev_id = 1;

	const char *uri = (const char *)key;
	if ((uri == NULL) || (iv == NULL)) {
		ERROR("PKCS#11 URI or AES IV missing for decryption!");
		return NULL;
	}

	/* Temporary to remove warning */
	cipher = cipher;

	dgst = calloc(1, sizeof(*dgst));
	if (!dgst) {
		return NULL;
	}

	dgst->p11uri = p11_kit_uri_new();
	err = p11_kit_uri_parse(uri, P11_KIT_URI_FOR_ANY, dgst->p11uri);
	if (err) {
		msg = p11_kit_uri_message(err);
		ERROR("PKCS#11 URI: %s", msg);
		free(dgst);
		return NULL;
	}

	slot_id = p11_kit_uri_get_slot_id(dgst->p11uri);
	key_id  = p11_kit_uri_get_attribute(dgst->p11uri, CKA_ID);
	pin     = p11_kit_uri_get_pin_value(dgst->p11uri);
	library = p11_kit_uri_get_module_path(dgst->p11uri);
	if (slot_id == -1 || key_id == NULL || pin == NULL || library == NULL) {
		ERROR("PKCS#11 URI must contain slot-id, id, pin-value, and module-path.");
		goto err_free;
	}

	// Set up a valid PKCS#7 block plus one state octet
	for (int i = 0; i <= AES_BLK_SIZE; i++) {
		dgst->last_decr[i] = AES_BLK_SIZE;
	}

#ifdef DEBUG_WOLFSSL
	wolfSSL_SetLoggingCb(wolfssl_debug);
	wolfSSL_Debugging_ON();
#endif
	wolfCrypt_Init();
	err = wc_Pkcs11_Initialize(&dgst->pkdev, library, NULL);
	if (err)
		goto err_msg;

	err = wc_Pkcs11Token_Init(&dgst->pktoken, &dgst->pkdev, slot_id,
					"unspecified", (unsigned char *)pin, strlen(pin));
	if (err)
		goto err_msg;

	err = wc_Pkcs11Token_Open(&dgst->pktoken, 0);
	if (err)
		goto err_msg;

	err = wc_CryptoCb_RegisterDevice(dev_id, wc_Pkcs11_CryptoDevCb, &dgst->pktoken);
	if (err)
		goto err_msg;

	err = wc_AesInit_Id(&dgst->ctxdec, key_id->pValue, key_id->ulValueLen, NULL, dev_id);
	if (err)
		goto err_msg;

	err = wc_AesSetIV(&dgst->ctxdec, iv);
	if (err)
		goto err_msg;

	INFO("PKCS#11 key set up successfully.");
	return dgst;

err_msg:
	msg = wc_GetErrorString(err);
	ERROR("PKCS#11 initialization failed: %s", msg);

err_free:
	wc_Pkcs11Token_Final(&dgst->pktoken);
	wc_Pkcs11_Finalize(&dgst->pkdev);

	p11_kit_uri_free(dgst->p11uri);
	free(dgst);

	return NULL;
}

static int wolfssl_DECRYPT_update(void *ctx, unsigned char *buf,
				int *outlen, const unsigned char *cryptbuf, int inlen)
{
	struct wolfssl_digest *dgst = (struct wolfssl_digest *)ctx;
	// precondition: len(buf) >= inlen + AES_BLK_SIZE
	unsigned char *pad_buf = &buf[AES_BLK_SIZE];
	const char *msg;
	int err;
	int one_off_sz = inlen - AES_BLK_SIZE;

	if (inlen < AES_BLK_SIZE)
		return -EFAULT;

	if (dgst->last_decr[AES_BLK_SIZE]) {
		// This is for the first decryption operation
		pad_buf = buf;
		dgst->last_decr[AES_BLK_SIZE] = 0;
		*outlen = one_off_sz;
	} else {
		memcpy(buf, dgst->last_decr, AES_BLK_SIZE);
		*outlen = inlen;
	}

	err = wc_AesCbcDecrypt(&dgst->ctxdec, pad_buf, cryptbuf, inlen);
	if (err) {
		msg = wc_GetErrorString(err);
		ERROR("PKCS#11 AES decryption failed: %s", msg);
		return -EFAULT;
	}
	// Remember the last decrypted block which might contain padding
	memcpy(dgst->last_decr, &pad_buf[one_off_sz], AES_BLK_SIZE);

	wc_AesSetIV(&dgst->ctxdec, &cryptbuf[one_off_sz]);

	return 0;
}

// Gets rid of PKCS#7 padding
static int wolfssl_DECRYPT_final(void *ctx, unsigned char *buf, int *outlen)
{
	struct wolfssl_digest *dgst = (struct wolfssl_digest *)ctx;
	unsigned char last_oct = dgst->last_decr[AES_BLK_SIZE - 1];
	if (last_oct > AES_BLK_SIZE || last_oct == 0) {
#ifndef CONFIG_ENCRYPTED_IMAGES_HARDEN_LOGGING
		ERROR("AES: Invalid PKCS#7 padding.");
#endif
		return -EFAULT;
	}

	for (int i = 2; i <= last_oct; i++) {
		if (dgst->last_decr[AES_BLK_SIZE - i] != last_oct) {
#ifndef CONFIG_ENCRYPTED_IMAGES_HARDEN_LOGGING
			ERROR("AES: Invalid PKCS#7 padding.");
#endif
			return -EFAULT;
		}
	}

	*outlen = AES_BLK_SIZE - last_oct;
	memcpy(buf, dgst->last_decr, *outlen);

	return 0;
}

static void wolfssl_DECRYPT_cleanup(void *ctx)
{
	struct wolfssl_digest *dgst = (struct wolfssl_digest *)ctx;
	if (dgst) {
		wc_Pkcs11Token_Final(&dgst->pktoken);
		wc_Pkcs11_Finalize(&dgst->pkdev);
		p11_kit_uri_free(dgst->p11uri);

		free(dgst);
		dgst = NULL;
	}

	wolfCrypt_Cleanup();
}

__attribute__((constructor))
static void wolfssl_probe(void)
{
	wolfssl.DECRYPT_init = wolfssl_DECRYPT_init;
	wolfssl.DECRYPT_update = wolfssl_DECRYPT_update;
	wolfssl.DECRYPT_final = wolfssl_DECRYPT_final;
	wolfssl.DECRYPT_cleanup = wolfssl_DECRYPT_cleanup;
	(void)register_cryptolib("wolfssl", &wolfssl);
}
