// SPDX-FileCopyrightText: 2024 Matej Zachar
//
// SPDX-License-Identifier: GPL-2.0-only
/*
 * Inspired by the wolfssl implementation done by Bastian Germann
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "swupdate_crypto.h"
#include "swupdate_pkcs11.h"
#include "util.h"

static swupdate_decrypt_lib pkcs11;

static CK_SLOT_ID find_slot(CK_FUNCTION_LIST_PTR module, P11KitUri *uri)
{
	CK_RV rv;

	CK_SLOT_ID slot_id = p11_kit_uri_get_slot_id(uri);
	if (slot_id != (CK_SLOT_ID)-1)
		return slot_id;

	size_t slot_count;
	rv = module->C_GetSlotList(1, NULL_PTR, &slot_count);
	if (rv != CKR_OK)
		return (CK_SLOT_ID)-1;

	CK_SLOT_ID slot_ids[slot_count];
	rv = module->C_GetSlotList(1, &slot_ids[0], &slot_count);
	if (rv != CKR_OK)
		return (CK_SLOT_ID)-1;

	CK_TOKEN_INFO token_info;
	for (int i = 0; i < slot_count; ++i) {
		slot_id = slot_ids[i];

		rv = module->C_GetTokenInfo(slot_id, &token_info);
		if (rv != CKR_OK)
			return (CK_SLOT_ID)-1;

		if (p11_kit_uri_match_token_info(uri, &token_info))
			return slot_id;
	}

	return (CK_SLOT_ID)-1;
}

static CK_RV find_key(CK_FUNCTION_LIST_PTR module, CK_SESSION_HANDLE session,
	CK_ATTRIBUTE_PTR key_id, CK_OBJECT_HANDLE *key_handle)
{
	CK_RV rv;

	CK_ATTRIBUTE find_template[] = {
		{ CKA_ID, key_id->pValue, key_id->ulValueLen }
	};

	rv = module->C_FindObjectsInit(session, find_template, 1);
	if (rv != CKR_OK) {
		return rv;
	}

	CK_ULONG object_count;
	rv = module->C_FindObjects(session, key_handle, 1, &object_count);
	if (rv != CKR_OK) {
		return rv;
	}

	rv = module->C_FindObjectsFinal(session);
	if (rv != CKR_OK) {
		return rv;
	}

	if (object_count == 0) {
		return CKR_DATA_INVALID;
	}

	return CKR_OK;
}

static void *pkcs11_DECRYPT_init(unsigned char *uri,
	char __attribute__ ((__unused__)) keylen, unsigned char *iv, cipher_t __attribute__ ((__unused__)) cipher)
{
	struct pkcs11_digest *dgst;
	CK_SLOT_ID slot_id;
	CK_ATTRIBUTE_PTR key_id;
	const char *pin;
	const char *module_path;
	const char *msg;
	int err = 0;
	CK_RV rv;

	if (uri == NULL || iv == NULL) {
		ERROR("PKCS#11 URI or AES IV missing for decryption!");
		return NULL;
	}

	dgst = calloc(1, sizeof(*dgst));
	if (!dgst) {
		return NULL;
	}

	dgst->uri = p11_kit_uri_new();
	err = p11_kit_uri_parse((const char*)uri, P11_KIT_URI_FOR_OBJECT_ON_TOKEN_AND_MODULE, dgst->uri);
	if (err) {
		msg = p11_kit_uri_message(err);
		ERROR("PKCS#11 URI: %s", msg);
		goto free_digest;
	}

	key_id	= p11_kit_uri_get_attribute(dgst->uri, CKA_ID);
	pin	= p11_kit_uri_get_pin_value(dgst->uri);
	module_path = p11_kit_uri_get_module_path(dgst->uri);
	if (key_id == NULL || pin == NULL || module_path == NULL) {
		ERROR("PKCS#11 URI must contain id, pin-value and module-path.");
		goto free_digest;
	}

	dgst->module = p11_kit_module_load(module_path, 0);
	if (dgst->module == NULL) {
		msg = p11_kit_message();
		ERROR("Failed to load PKCS#11 module [%s]: %s\n", module_path, msg);
		goto free_digest;
	}

	rv = dgst->module->C_Initialize(NULL_PTR);
	if (rv != CKR_OK)
		goto err_msg;

	slot_id = find_slot(dgst->module, dgst->uri);
	if (slot_id == -1) {
		ERROR("PKCS#11 URI must contain slot-id or token identification such as token, model, serial, manufacturer.");
		goto free_digest;
	}

	rv = dgst->module->C_OpenSession(slot_id, CKF_SERIAL_SESSION | CKF_RW_SESSION, NULL_PTR, NULL_PTR, &dgst->session);
	if (rv != CKR_OK)
		goto err_msg;

	rv = dgst->module->C_Login(dgst->session, CKU_USER, (unsigned char *)pin, strnlen(pin, 32));
	if (rv != CKR_OK)
		goto err_msg;

	CK_OBJECT_HANDLE key;
	rv = find_key(dgst->module, dgst->session, key_id, &key);
	if (rv != CKR_OK)
		goto err_msg;

	// Setup a valid PKCS#7 block plus one state octet
	for (int i = 0; i <= AES_BLK_SIZE; ++i) {
		dgst->last[i] = AES_BLK_SIZE;
	}

	// Setup IV vector & mechanism
	memcpy(dgst->iv, iv, AES_BLK_SIZE);
	dgst->mechanism.mechanism = CKM_AES_CBC;
	dgst->mechanism.pParameter = dgst->iv;
	dgst->mechanism.ulParameterLen = AES_BLK_SIZE;

	rv = dgst->module->C_DecryptInit(dgst->session, &dgst->mechanism, key);
	if (rv != CKR_OK)
		goto err_msg;

	INFO("PKCS#11 key set up successfully.");
	return dgst;

err_msg:
	msg = p11_kit_strerror(rv);
	ERROR("PKCS#11 initialization failed: %s", msg);

free_digest:
	if (dgst->uri)
		p11_kit_uri_free(dgst->uri);

	if (dgst->session)
		dgst->module->C_CloseSession(dgst->session);

	if (dgst->module) {
		dgst->module->C_Finalize(NULL_PTR);
		p11_kit_module_release(dgst->module);
	}

	free(dgst);

	return NULL;
}

static int pkcs11_DECRYPT_update(void *ctx, unsigned char *buf,
	int *outlen, const unsigned char *cryptbuf, int inlen)
{
	struct pkcs11_digest *dgst = (struct pkcs11_digest *)ctx;
	// precondition: len(buf) >= inlen + AES_BLK_SIZE
	unsigned long buf_len = inlen + AES_BLK_SIZE;
	CK_RV rv;

	if (inlen < AES_BLK_SIZE)
		return -EFAULT;

	if (dgst->last[AES_BLK_SIZE]) {
		dgst->last[AES_BLK_SIZE] = 0;
		// first run - there is no block to append
		*outlen = 0;
	} else {
		// append previously decrypted last AES block
		memcpy(buf, dgst->last, AES_BLK_SIZE);
		buf += AES_BLK_SIZE;
		*outlen = AES_BLK_SIZE;
	}

	rv = dgst->module->C_DecryptUpdate(dgst->session, (unsigned char*)cryptbuf, inlen, buf, &buf_len);
	if (rv != CKR_OK) {
		ERROR("PKCS#11 AES decryption failed: %s", p11_kit_strerror(rv));
		return -EFAULT;
	}

	// strip and remember last AES block from decoded buffer
	// it will get appended either in the next call to DECRYPT_update or DECRYPT_final
	buf_len -= AES_BLK_SIZE;
	memcpy(dgst->last, &buf[buf_len], AES_BLK_SIZE);

	// update iv for the next block
	memcpy(dgst->iv, cryptbuf + inlen - AES_BLK_SIZE, AES_BLK_SIZE);

	*outlen += (int)buf_len;
	return 0;
}

static int pkcs11_DECRYPT_final(void *ctx, unsigned char *buf, int *outlen)
{
	struct pkcs11_digest *dgst = (struct pkcs11_digest *)ctx;
	CK_RV rv;
	unsigned long extra_len = 0;

	if (dgst->last[AES_BLK_SIZE]) {
#ifndef CONFIG_ENCRYPTED_IMAGES_HARDEN_LOGGING
		ERROR("AES: At least one call to pkcs11_DECRYPT_update was expected");
#endif
		return -EINVAL;
	}

	// append previously decrypted last AES block if any
	memcpy(buf, dgst->last, AES_BLK_SIZE);

	rv = dgst->module->C_DecryptFinal(dgst->session, &buf[AES_BLK_SIZE], &extra_len);
	if (rv != CKR_OK)
		return -EFAULT;

	// obtain last AES block after C_DecryptFinal
	CK_BYTE_PTR last = &buf[extra_len];

	// Handle manual PKCS#7 padding removal
	CK_BYTE padding_value = last[AES_BLK_SIZE - 1];

	if (padding_value <= 0 || padding_value > AES_BLK_SIZE) {
#ifndef CONFIG_ENCRYPTED_IMAGES_HARDEN_LOGGING
		ERROR("AES: Invalid PKCS#7 padding value [%u]", padding_value);
#endif
		return -EFAULT;
	}

	// Verify that padding is correct
	for (CK_BYTE i = 0; i < padding_value; ++i) {
		if (last[AES_BLK_SIZE - 1 - i] != padding_value) {
#ifndef CONFIG_ENCRYPTED_IMAGES_HARDEN_LOGGING
			ERROR("AES: Invalid PKCS#7 padding value [%u] at offset %u", padding_value, i);
#endif
			return -EINVAL;
		}
	}

	*outlen = (int)extra_len + AES_BLK_SIZE - padding_value;
	return 0;
}

static void pkcs11_DECRYPT_cleanup(void *ctx)
{
	struct pkcs11_digest *dgst = (struct pkcs11_digest *)ctx;
	if (dgst) {
		if (dgst->uri)
			p11_kit_uri_free(dgst->uri);

		if (dgst->session)
			dgst->module->C_CloseSession(dgst->session);

		if (dgst->module) {
			dgst->module->C_Finalize(NULL_PTR);
			p11_kit_module_release(dgst->module);
		}

		free(dgst);
		dgst = NULL;
	}
}

__attribute__((constructor))
static void pkcs11_probe(void)
{
	pkcs11.DECRYPT_init = pkcs11_DECRYPT_init;
	pkcs11.DECRYPT_update = pkcs11_DECRYPT_update;
	pkcs11.DECRYPT_final = pkcs11_DECRYPT_final;
	pkcs11.DECRYPT_cleanup = pkcs11_DECRYPT_cleanup;
	(void)register_cryptolib("pkcs11", &pkcs11);
}
