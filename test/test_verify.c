// SPDX-FileCopyrightText: 2019 Laszlo Ashin
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

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include "sslapi.h"
#include "swupdate.h"

#define DATADIR "test/data/"

static void test_verify_pkcs15(void **state)
{
	int error;
	struct swupdate_cfg config;

	(void)state;

	config.dgst = NULL;
	error = swupdate_dgst_init(&config, DATADIR "signing-pubkey.pem");
	assert_int_equal(error, 0);

	error = swupdate_verify_file(config.dgst, DATADIR "signature",
		DATADIR "to-be-signed", NULL);
	assert_int_equal(error, 0);
}

int main(void)
{
	swupdate_crypto_init();
	static const struct CMUnitTest verify_tests[] = {
		cmocka_unit_test(test_verify_pkcs15),
	};
	return cmocka_run_group_tests_name("verify", verify_tests, NULL, NULL);
}
