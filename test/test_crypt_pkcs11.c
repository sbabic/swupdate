// SPDX-FileCopyrightText: 2024 Matej Zachar
//
// SPDX-License-Identifier: GPL-2.0-only

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cmocka.h>
#include <util.h>
#include "swupdate_crypto.h"

#define BUFFER_SIZE (AES_BLK_SIZE * 1024)
#define TOKENDIR "test/data/token"

static unsigned char small_data[AES_BLK_SIZE];

static int read_file(const char *path, unsigned char *buffer, size_t *size)
{
	FILE *fp = fopen(path, "r");
	if (!fp) {
		fprintf(stderr, "Failed to open file '%s'\n", path);
		return -1;
	}

	size_t len = fread(buffer, sizeof(char), *size, fp);
	if (ferror(fp) != 0) {
		fprintf(stderr, "Error reading file '%s'\n", path);
		fclose(fp);
		return -1;
	}

	*size = len;
	fclose(fp);

	return 0;
}

static void test_crypt_pkcs11_256(void **state)
{
	(void) state;
	int err;

	const char * uri = "pkcs11:token=TestToken;id=%A1%B2?pin-value=1234&module-path=/usr/lib/softhsm/libsofthsm2.so";

	size_t original_data_len = 128 * 1024;/* 128KiB */
	unsigned char original_data[original_data_len];
	err = read_file(TOKENDIR "/original.data", &original_data[0], &original_data_len);
	assert_true(err == 0);

	size_t encrypted_data_len = 128 * 1024 + AES_BLK_SIZE;/* 128KiB AES_BLK_SIZE(16B) */
	unsigned char encrypted_data[encrypted_data_len];
	err = read_file(TOKENDIR "/encrypted.data", &encrypted_data[0], &encrypted_data_len);
	assert_true(err == 0);

	unsigned char decrypted_data[encrypted_data_len];

	size_t iv_size = 16;
	unsigned char iv[iv_size];
	err = read_file(TOKENDIR "/encrypted.data.iv", &iv[0], &iv_size);
	assert_true(err == 0);

	unsigned char buffer[BUFFER_SIZE + AES_BLK_SIZE];

	set_cryptolib("pkcs11");
	struct swupdate_digest *dgst = swupdate_DECRYPT_init((unsigned char *)uri, 0, &iv[0], AES_CBC_256);
	assert_non_null(dgst);

	int len;
	size_t e_offset = 0;
	size_t d_offset = 0;
	while (e_offset < encrypted_data_len) {
		size_t chunk_size = (encrypted_data_len - e_offset > BUFFER_SIZE) ? BUFFER_SIZE : encrypted_data_len - e_offset;

		err = swupdate_DECRYPT_update(dgst, buffer, &len, encrypted_data + e_offset, chunk_size);
		assert_true(err == 0);
		assert_true(len >= AES_BLK_SIZE && len <= chunk_size);
		e_offset += chunk_size;

		memcpy(&decrypted_data[d_offset], buffer, len);
		d_offset += len;
	}

	err = swupdate_DECRYPT_final(dgst, buffer, &len);
	assert_true(err == 0);
	assert_true(len == 3); /* as the size is 128*1024+3 */

	memcpy(&decrypted_data[d_offset], buffer, len);
	d_offset += len;

	assert_true(strncmp((const char *)decrypted_data, (const char *)original_data, original_data_len) == 0);
}

static size_t captured_len = 0;

static int capture_output(void *out, const void *buf, size_t len)
{
	(void)out;
	assert_true(captured_len + len <= sizeof(small_data));
	memcpy(&small_data[captured_len], buf, len);
	captured_len += len;
	return 0;
}

/*
 * Regression test: decrypting a file that fits into a single AES block
 * must yield its plaintext. Files of 16 bytes or less (e.g. a version
 * string) have their whole plaintext buffered inside the decrypt context
 * until the final call, so the copy pipeline must not stop early.
 */
static void test_crypt_pkcs11_single_block(void **state)
{
	(void) state;
	int err;

	static const char *uri =
		"pkcs11:token=TestToken;id=%A1%B2?pin-value=1234&module-path=/usr/lib/softhsm/libsofthsm2.so";

	static const char version[] = "1.2.3\n";

	err = set_aes_key(uri, "c1f390d21dd06118cbd333144a3318ca");
	assert_true(err == 0);

	struct stat sb;
	err = stat(TOKENDIR "/small.encrypted.data", &sb);
	assert_true(err == 0);
	assert_true(sb.st_size == AES_BLK_SIZE);

	unsigned long offs = 0;
	int fdin = open(TOKENDIR "/small.encrypted.data", O_RDONLY);
	assert_true(fdin >= 0);

	struct swupdate_copy copy = {
		.fdin = fdin,
		.out = NULL,
		.callback = capture_output,
		.nbytes = sb.st_size,
		.offs = &offs,
		.encrypted = true,
		.cipher = AES_CBC,
	};

	captured_len = 0;
	err = copyfile(&copy);
	close(fdin);
	assert_true(err == 0);

	assert_true(captured_len == sizeof(version) - 1);
	assert_true(memcmp(small_data, version, captured_len) == 0);
}

int main(void)
{
	const struct CMUnitTest crypt_pkcs11_tests[] = {
		cmocka_unit_test(test_crypt_pkcs11_256),
		cmocka_unit_test(test_crypt_pkcs11_single_block)
	};
	return cmocka_run_group_tests_name("crypt_pkcs11", crypt_pkcs11_tests, NULL, NULL);
}
