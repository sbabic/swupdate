/*
 * (C) Copyright 2025
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 *
 */

#pragma once
#include <stdbool.h>
#include <string.h>

/*
 * Add global definitions to set the cipher.
 * Each implementation must map the generic enum
 * to the library specific code
 */

typedef enum {
	AES_CBC,
	AES_CBC_128,
	AES_CBC_192,
	AES_CBC_256,
	AES_CTR,
	AES_CTR_128,
	AES_CTR_192,
	AES_CTR_256,
	CMS,
	AES_UNKNOWN
} cipher_t;

typedef struct {
	cipher_t cipher;
	const char *ciphername;
} map_cipher_t;

static map_cipher_t map_cipher[] = {
	{AES_CBC, "aes-cbc"},
	{AES_CBC_128, "aes-cbc-128"},
	{AES_CBC_192, "aes-cbc-192"},
	{AES_CBC_256, "aes-cbc-256"},
	{AES_CBC_128, "aes-cbc-128"},
	{AES_CTR_128, "aes-ctr-128"},
	{AES_CTR_192, "aes-ctr-192"},
	{AES_CTR_256, "aes-ctr-256"},
	{AES_CTR_128, "aes-ctr-128"}
};

static inline cipher_t map_name_cipher (const char *name) {
	for (int count = 0; count < sizeof(map_cipher)/sizeof(map_cipher_t); count++) {
		if (!strcmp(map_cipher[count].ciphername, name))
			return map_cipher[count].cipher;
	}
	return AES_UNKNOWN;
}
