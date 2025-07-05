/*
 * (C) Copyright 2024
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 *
 */

#include <stdlib.h>
#include <errno.h>
#include <util.h>
#include "swupdate_crypto.h"

typedef enum {
	DECRYPTLIB,
	HASHLIB,
	DGSTLIB
} crypto_lib_t;

#define NUMLIBS	(DGSTLIB + 1)

const char *libdesc[] = {
	"decrypt",
	"hash",
	"verify"
};

/*
 * Reused from bootloader
 */
typedef struct {
	const char *name;
	void *lib;
} entry;

static entry *current[NUMLIBS] = {NULL, NULL,  NULL};
static entry *available[NUMLIBS] = {NULL, NULL, NULL};
static unsigned int num_available [] = {0 , 0, 0};

static int register_lib(const char *name, crypto_lib_t type, void *lib)
{
	int num = num_available[type];
	entry *avail = available[type];
	entry *tmp = realloc(avail, (num + 1) * sizeof(entry));
	if (!tmp) {
		return -ENOMEM;
	}
	tmp[num].name = (char*)name;
	tmp[num].lib = lib;
	num_available[type]++;
	available[type] = tmp;
	current[type] = available[type];
	return 0;
}

static int setlib(const char *name, crypto_lib_t type)
{
	int num = num_available[type];
	entry *elem;

	if (!name) {
		return -ENOENT;
	}
	elem = available[type];
	for (unsigned int i = 0; i < num; i++) {
		if (elem[i].lib &&
		    (strcmp(elem[i].name, name) == 0)) {
			current[type] = &elem[i];
			return 0;
		}
	}
	return -ENOENT;
}

static const char* getlib(crypto_lib_t type)
{
	return current[type] ? current[type]->name : NULL;
}

int register_cryptolib(const char *name, swupdate_decrypt_lib *lib)
{
	return register_lib(name, DECRYPTLIB, lib);
}

int register_hashlib(const char *name, swupdate_HASH_lib *lib)
{
	return register_lib(name, HASHLIB, lib);
}

int register_dgstlib(const char *name, swupdate_dgst_lib *lib)
{
	return register_lib(name, DGSTLIB, lib);
}

int set_cryptolib(const char *name)
{
	return setlib(name, DECRYPTLIB);
}

int set_HASHlib(const char *name)
{
	return setlib(name, HASHLIB);
}

int set_dgstlib(const char *name)
{
	return setlib(name, DGSTLIB);
}

const char* get_cryptolib(void)
{
	return getlib(DECRYPTLIB);
}

const char* get_HASHlib(void)
{
	return getlib(HASHLIB);
}

const char* get_dgstlib(void)
{
	return getlib(DGSTLIB);
}

void print_registered_cryptolib(void)
{
	INFO("Registered Crypto Providers:");

	for (int type = 0; type < NUMLIBS; type++) {
		int num = num_available[type];
		entry *elem = available[type];
		entry *cur = current[type];
		if (num > 0) {
			INFO("\tProvider for %s", libdesc[type]);
		}
		for (unsigned int i = 0; i < num; i++) {
			INFO("\t\t%s%s", elem[i].name, cur == &elem[i] ? "*" : "");
		}
	}
}

struct swupdate_digest *swupdate_DECRYPT_init(unsigned char *key, char keylen, unsigned char *iv)
{
	swupdate_decrypt_lib *lib;
	if (!get_cryptolib())
		return NULL;

	lib = (swupdate_decrypt_lib *)current[DECRYPTLIB]->lib;
	return lib->DECRYPT_init(key, keylen, iv);
}

int swupdate_DECRYPT_update(struct swupdate_digest *dgst, unsigned char *buf, 
				int *outlen, const unsigned char *cryptbuf, int inlen)
{
	swupdate_decrypt_lib *lib;
	if (!get_cryptolib())
		return -EINVAL;

	lib = (swupdate_decrypt_lib *)current[DECRYPTLIB]->lib;
	return lib->DECRYPT_update(dgst, buf, outlen, cryptbuf, inlen);
}

int swupdate_DECRYPT_final(struct swupdate_digest *dgst, unsigned char *buf, int *outlen)
{
	swupdate_decrypt_lib *lib;
	if (!get_cryptolib())
		return -EINVAL;
	lib = (swupdate_decrypt_lib *)current[DECRYPTLIB]->lib;
	return lib->DECRYPT_final(dgst, buf, outlen);
}

void swupdate_DECRYPT_cleanup(struct swupdate_digest *dgst)
{
	swupdate_decrypt_lib *lib;
	if (!get_cryptolib())
		return;
	lib = (swupdate_decrypt_lib *)current[DECRYPTLIB]->lib;
	return lib->DECRYPT_cleanup(dgst);
}

struct swupdate_digest *swupdate_HASH_init(const char *SHAlength)
{
	swupdate_HASH_lib *lib;

	if (!get_HASHlib())
		return NULL;
	lib = (swupdate_HASH_lib *)current[HASHLIB]->lib;

	return lib->HASH_init(SHAlength);
}

int swupdate_HASH_update(struct swupdate_digest *dgst, const unsigned char *buf, size_t len)
{
	swupdate_HASH_lib *lib;

	if (!get_HASHlib())
		return -EFAULT;
	lib = (swupdate_HASH_lib *)current[HASHLIB]->lib;

	return lib->HASH_update(dgst, buf, len);
}

int swupdate_HASH_final(struct swupdate_digest *dgst, unsigned char *md_value, unsigned int *md_len)
{
	swupdate_HASH_lib *lib;

	if (!get_HASHlib())
		return -EFAULT;
	lib = (swupdate_HASH_lib *)current[HASHLIB]->lib;

	return lib->HASH_final(dgst, md_value, md_len);
}

int swupdate_HASH_compare(const unsigned char *hash1, const unsigned char *hash2)
{
	swupdate_HASH_lib *lib;

	if (!get_HASHlib())
		return -EFAULT;
	lib = (swupdate_HASH_lib *)current[HASHLIB]->lib;

	return lib->HASH_compare(hash1, hash2);
}

void swupdate_HASH_cleanup(struct swupdate_digest *dgst)
{
	swupdate_HASH_lib *lib;

	if (!get_HASHlib())
		return;
	lib = (swupdate_HASH_lib *)current[HASHLIB]->lib;

	lib->HASH_cleanup(dgst);
}

int swupdate_dgst_init(struct swupdate_cfg *sw, const char *keyfile)
{
	swupdate_dgst_lib *lib;

	if (!get_dgstlib())
		return -EFAULT;
	lib = (swupdate_dgst_lib *)current[DGSTLIB]->lib;

	return lib->dgst_init(sw, keyfile);
}

int swupdate_verify_file(struct swupdate_digest *dgst, const char *sigfile,
		const char *file, const char *signer_name)
{
	swupdate_dgst_lib *lib;

	if (!get_dgstlib())
		return -EFAULT;
	lib = (swupdate_dgst_lib *)current[DGSTLIB]->lib;

	return lib->verify_file(dgst, sigfile, file, signer_name);
}
