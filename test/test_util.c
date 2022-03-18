// SPDX-FileCopyrightText: 2022 Kyle Russell <bkylerussell@gmail.com>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "util.h"

static int util_setup(void **state)
{
	(void)state;
	return 0;
}

static int util_teardown(void **state)
{
	(void)state;
	return 0;
}

static void test_util_size_delimiter_match(void **state)
{
	(void)state;
	assert_int_equal(size_delimiter_match("1024G, some fancy things"), 1);
	assert_int_equal(size_delimiter_match("2048KiB"), 1);
	assert_int_equal(size_delimiter_match("1073741824"), 0);
}

static void test_util_ustrtoull(void **state)
{
	(void)state;
	char *suffix = NULL;
	uint64_t size = ustrtoull("1024M, some fancy things", &suffix, 10);
	assert_int_equal(errno, 0);
	assert_int_equal(size, 1073741824);
	assert_string_equal(suffix, ", some fancy things");
}

int main(void)
{
	int error_count = 0;
	const struct CMUnitTest util_tests[] = {
	    cmocka_unit_test(test_util_ustrtoull),
	    cmocka_unit_test(test_util_size_delimiter_match)
	};
	error_count += cmocka_run_group_tests_name("util", util_tests,
						   util_setup, util_teardown);
	return error_count;
}
