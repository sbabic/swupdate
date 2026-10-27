/*
 * (C) Copyright 2026
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 *
 * Key handling and signing for Suricatta Lua modules, for servers
 * that authenticate a device by a signed request (as Mender does):
 *
 *   suricatta.crypto.genkey(path, type [, bits])
 *   suricatta.crypto.pubkey(path)
 *   suricatta.crypto.sign(path, data)
 *
 * type is "rsa" (default 3072 bit), "ec" (P-256) or "ed25519".
 * Signatures are Base64 encoded: RSA PKCS#1 v1.5 and ECDSA (ASN.1) over
 * SHA-256 of data, Ed25519 over data.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lua.h>
#include <lauxlib.h>
#include <lua_compat.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ec.h>
#include <openssl/objects.h>

#include "util.h"
#include "suricatta/server.h"

#if OPENSSL_VERSION_NUMBER < 0x10101000L
#error "suricatta.crypto needs OpenSSL 1.1.1 or newer"
#endif

#define RSA_DEFAULT_BITS	3072
#define RSA_MIN_BITS		2048

void suricatta_lua_crypto_register(lua_State *L);

static int push_error(lua_State *L, const char *what)
{
	char buf[256];
	unsigned long err = ERR_get_error();

	if (err) {
		ERR_error_string_n(err, buf, sizeof(buf));
		lua_pushnil(L);
		lua_pushfstring(L, "%s: %s", what, buf);
	} else {
		lua_pushnil(L);
		lua_pushstring(L, what);
	}
	ERR_clear_error();

	return 2;
}

static EVP_PKEY *load_key(const char *path)
{
	EVP_PKEY *pkey = NULL;
	FILE *fp = fopen(path, "r");

	if (!fp)
		return NULL;
	pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
	fclose(fp);

	return pkey;
}

/*
 * Write the key to a temporary file readable by the owner only, then
 * rename it, so that a crash never leaves a partial key behind.
 */
static int write_key(const char *path, EVP_PKEY *pkey)
{
	char *tmp = NULL;
	FILE *fp;
	int fd, ret = -1;

	if (asprintf(&tmp, "%s.tmp", path) == ENOMEM_ASPRINTF)
		return -1;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		goto out;
	fp = fdopen(fd, "w");
	if (!fp) {
		close(fd);
		goto out_unlink;
	}
	if (PEM_write_PrivateKey(fp, pkey, NULL, NULL, 0, NULL, NULL) != 1 ||
	    fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
		fclose(fp);
		goto out_unlink;
	}
	if (fclose(fp) != 0)
		goto out_unlink;
	if (rename(tmp, path) != 0)
		goto out_unlink;
	ret = 0;
	goto out;

out_unlink:
	unlink(tmp);
out:
	free(tmp);
	return ret;
}

/**
 * @brief Generate a private key and store it in PEM format.
 *
 * @param  [Lua] Path of the key file, created with mode 0600.
 * @param  [Lua] Key type: "rsa", "ec" or "ed25519".
 * @param  [Lua] RSA key size in bits (optional, default 3072).
 * @return [Lua] True, or nil and an error message.
 */
static int lua_crypto_genkey(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	const char *type = luaL_checkstring(L, 2);
	int bits = (int)luaL_optinteger(L, 3, RSA_DEFAULT_BITS);
	EVP_PKEY_CTX *ctx = NULL;
	EVP_PKEY *pkey = NULL;
	int id, nret;

	if (!strcmp(type, "rsa")) {
		id = EVP_PKEY_RSA;
		if (bits < RSA_MIN_BITS) {
			lua_pushnil(L);
			lua_pushfstring(L, "RSA keys need at least %d bits", RSA_MIN_BITS);
			return 2;
		}
	} else if (!strcmp(type, "ec")) {
		id = EVP_PKEY_EC;
	} else if (!strcmp(type, "ed25519")) {
		id = EVP_PKEY_ED25519;
	} else {
		lua_pushnil(L);
		lua_pushfstring(L, "Unknown key type '%s'", type);
		return 2;
	}

	ctx = EVP_PKEY_CTX_new_id(id, NULL);
	if (!ctx || EVP_PKEY_keygen_init(ctx) != 1 ||
	    (id == EVP_PKEY_RSA && EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) != 1) ||
	    (id == EVP_PKEY_EC &&
	     EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) != 1) ||
	    EVP_PKEY_keygen(ctx, &pkey) != 1) {
		nret = push_error(L, "Cannot generate key");
		goto out;
	}

	if (write_key(path, pkey) != 0) {
		lua_pushnil(L);
		lua_pushfstring(L, "Cannot write key %s: %s", path, strerror(errno));
		nret = 2;
		goto out;
	}
	lua_pushboolean(L, 1);
	nret = 1;
out:
	EVP_PKEY_free(pkey);
	EVP_PKEY_CTX_free(ctx);
	return nret;
}

/**
 * @brief Get the public key of a private key file.
 *
 * @param  [Lua] Path of the private key file.
 * @return [Lua] Public key in PEM format, or nil and an error message.
 */
static int lua_crypto_pubkey(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	EVP_PKEY *pkey = load_key(path);
	BIO *bio = NULL;
	char *data;
	long len;
	int nret;

	if (!pkey)
		return push_error(L, "Cannot load key");

	bio = BIO_new(BIO_s_mem());
	if (!bio || PEM_write_bio_PUBKEY(bio, pkey) != 1) {
		nret = push_error(L, "Cannot export public key");
		goto out;
	}
	len = BIO_get_mem_data(bio, &data);
	lua_pushlstring(L, data, (size_t)len);
	nret = 1;
out:
	BIO_free(bio);
	EVP_PKEY_free(pkey);
	return nret;
}

/**
 * @brief Sign data with a private key.
 *
 * @param  [Lua] Path of the private key file.
 * @param  [Lua] Data to sign.
 * @return [Lua] Base64 encoded signature, or nil and an error message.
 */
static int lua_crypto_sign(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	size_t len;
	const char *data = luaL_checklstring(L, 2, &len);
	EVP_PKEY *pkey = load_key(path);
	EVP_MD_CTX *ctx = NULL;
	const EVP_MD *md;
	unsigned char *sig = NULL, *b64 = NULL;
	size_t siglen = 0;
	int nret;

	if (!pkey)
		return push_error(L, "Cannot load key");

	/* Ed25519 signs the message itself */
	md = EVP_PKEY_id(pkey) == EVP_PKEY_ED25519 ? NULL : EVP_sha256();
	ctx = EVP_MD_CTX_new();
	if (!ctx || EVP_DigestSignInit(ctx, NULL, md, NULL, pkey) != 1 ||
	    EVP_DigestSign(ctx, NULL, &siglen, (const unsigned char *)data, len) != 1) {
		nret = push_error(L, "Cannot sign");
		goto out;
	}
	sig = malloc(siglen);
	b64 = malloc(4 * ((siglen + 2) / 3) + 1);
	if (!sig || !b64) {
		lua_pushnil(L);
		lua_pushstring(L, "Out of memory");
		nret = 2;
		goto out;
	}
	if (EVP_DigestSign(ctx, sig, &siglen, (const unsigned char *)data, len) != 1) {
		nret = push_error(L, "Cannot sign");
		goto out;
	}
	EVP_EncodeBlock(b64, sig, (int)siglen);
	lua_pushstring(L, (const char *)b64);
	nret = 1;
out:
	free(sig);
	free(b64);
	EVP_MD_CTX_free(ctx);
	EVP_PKEY_free(pkey);
	return nret;
}

/**
 * @brief Add the 'crypto' Table to the 'suricatta' module Table on stack top.
 */
void suricatta_lua_crypto_register(lua_State *L)
{
	luaL_Reg lua_funcs_crypto[] = {
		{ "genkey", lua_crypto_genkey },
		{ "pubkey", lua_crypto_pubkey },
		{ "sign",   lua_crypto_sign   },
		{ NULL, NULL }
	};

	lua_pushstring(L, "crypto");
	lua_newtable(L);
	luaL_setfuncs(L, lua_funcs_crypto, 0);
	lua_settable(L, -3);
}
