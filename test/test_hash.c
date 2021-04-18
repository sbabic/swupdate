// SPDX-FileCopyrightText: 2019 Laszlo Ashin <laszlo@ashin.hu>
//
// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#include <stddef.h>
#include <setjmp.h>
#include <stdarg.h>
#include <cmocka.h>
#include <string.h>

#include "sslapi.h"
#include "util.h"

struct testvector {
	const char *input;
	const char *sha1;
	const char *sha256;
};

// https://www.di-mgt.com.au/sha_testvectors.html
static const struct testvector testvectors[] = {
	{
		.input = "abc",
		.sha1 = "a9993e364706816aba3e25717850c26c9cd0d89d",
		.sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
	},
	{
		.input = "",
		.sha1 = "da39a3ee5e6b4b0d3255bfef95601890afd80709",
		.sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
	},
	{
		.input = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		.sha1 = "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
		.sha256 = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
	},
	{
		.input = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
		.sha1 = "a49b2446a02c645bf419f995b67091253a04a259",
		.sha256 = "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
	},
};

static void hex2bin(unsigned char *dest, const unsigned char *source)
{
	unsigned int val;
	for (unsigned int i = 0; i < strlen((const char *)source); i += 2) {
		val = from_ascii((const char *)&source[i], 2, LG_16);
		dest[i / 2] = val;
	}
}

static void do_concrete_hash(const char* algo, const char* input, const char* expected_hex)
{
	int error;
	uint8_t result[32] = {0};
	unsigned len = 0;
	uint8_t expected_bin[32] = {0};
	struct swupdate_digest *dgst;

	dgst = swupdate_HASH_init(algo);
	assert_non_null(dgst);
	error = swupdate_HASH_update(dgst, (uint8_t *)input, strlen(input));
	assert_true(!error);

	error = swupdate_HASH_final(dgst, result, &len);
	assert_int_equal(error, 1);
	assert_int_equal(len, strlen(expected_hex) / 2);

	swupdate_HASH_cleanup(dgst);

	hex2bin(expected_bin, (uint8_t *)expected_hex);
	error = swupdate_HASH_compare(expected_bin, result);
	assert_true(!error);
}

static void do_hash(const struct testvector *vector)
{
	do_concrete_hash("sha1", vector->input, vector->sha1);
	do_concrete_hash("sha256", vector->input, vector->sha256);
}

static void test_hash_vectors(void **state)
{
	unsigned i;

	(void)state;

	for (i = 0; i < sizeof(testvectors) / sizeof(testvectors[0]); ++i) {
		do_hash(testvectors + i);
	}
}

static void test_hash_compare(void **state)
{
	(void)state;

	static const uint8_t a[32] = {0};
	static const uint8_t b[32] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};

	assert_int_equal(swupdate_HASH_compare(a, a), 0);
	assert_int_equal(swupdate_HASH_compare(a, b), -1);
}

int main(void)
{
	static const struct CMUnitTest hash_tests[] = {
		cmocka_unit_test(test_hash_compare),
		cmocka_unit_test(test_hash_vectors),
	};
	return cmocka_run_group_tests_name("hash", hash_tests, NULL, NULL);
}
