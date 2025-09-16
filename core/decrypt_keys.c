/*
 * (C) Copyright 2025
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "util.h"
#include "generated/autoconf.h"

/*
 * key    is 256 bit for max aes_256
 *	  or is a pkcs#11 URL
 * keylen is the actual aes key length
 * ivt    is 128 bit
 */
struct decryption_key {
	char *key;
	char keylen;
	unsigned char ivt[AES_BLK_SIZE];
};

static struct decryption_key *decrypt_keys = NULL;

int set_aes_key(const char *key, const char *ivt)
{
	int ret;
	size_t keylen;
	bool is_pkcs11 = false;

	/*
	 * Allocates the global structure just once
	 */
	if (!decrypt_keys) {
		decrypt_keys = (struct decryption_key *)calloc(1, sizeof(*decrypt_keys));
		if (!decrypt_keys)
			return -ENOMEM;
	}

	if (strlen(ivt) != (AES_BLK_SIZE*2) || !is_hex_str(ivt)) {
		ERROR("Invalid ivt");
		return -EINVAL;
	}

	ret = ascii_to_bin(decrypt_keys->ivt, sizeof(decrypt_keys->ivt), ivt);
	keylen = strlen(key);

	if (!strcmp("pkcs11", key)) {
		is_pkcs11 = true;
		decrypt_keys->keylen = keylen;

	} else {
		switch (keylen) {
		case AES_128_KEY_LEN * 2:
		case AES_192_KEY_LEN * 2:
		case AES_256_KEY_LEN * 2:
			// valid hex string size for AES 128/192/256
			decrypt_keys->keylen = keylen / 2;
			break;
		default:
			ERROR("Invalid decrypt_keys length");
			return -EINVAL;
		}
	}

	if (decrypt_keys->key)
		free(decrypt_keys->key);

	decrypt_keys->key = calloc(1, keylen + 1);
	if (!decrypt_keys->key)
		return -ENOMEM;

	if (is_pkcs11) {
		strncpy(decrypt_keys->key, key, keylen);
	} else {
		ret |= !is_hex_str(key);
		ret |= ascii_to_bin((unsigned char *)decrypt_keys->key, decrypt_keys->keylen, key);
	}

	if (ret) {
		ERROR("Invalid decrypt_keys");
		return -EINVAL;
	}

	return 0;
}

int load_decryption_key(char *fname)
{
	FILE *fp;
	char *b1 = NULL, *b2 = NULL;
	int ret;

	fp = fopen(fname, "r");
	if (!fp)
		return -EBADF;

	ret = fscanf(fp, "%ms %ms", &b1, &b2);
	switch (ret) {
		case 2:
			DEBUG("Read decryption key and initialization vector from file %s.", fname);
			break;
		default:
			if (b1 != NULL)
				free(b1);
			fprintf(stderr, "File with decryption key is not in the format <key> <ivt>\n");
			fclose(fp);
			return -EINVAL;
	}
	fclose(fp);

	ret = set_aes_key(b1, b2);

	if (b1 != NULL)
		free(b1);
	if (b2 != NULL)
		free(b2);

	if (ret) {
		fprintf(stderr, "Keys are invalid\n");
		return -EINVAL;
	}

	return 0;
}

char *swupdate_get_decrypt_key(void) {
	if (!decrypt_keys)
		return NULL;
	return decrypt_keys->key;
}

char swupdate_get_decrypt_keylen(void) {
	if (!decrypt_keys)
		return -1;
	return decrypt_keys->keylen;
}

unsigned char *get_aes_ivt(void) {
	if (!decrypt_keys)
		return NULL;
	return decrypt_keys->ivt;
}
