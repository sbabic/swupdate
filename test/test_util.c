// SPDX-FileCopyrightText: 2022 Kyle Russell <bkylerussell@gmail.com>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "swupdate_status.h"
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

static void test_util_is_filename_valid(void **state)
{
	(void)state;
	assert_true(is_filename_valid("a.swu"));
	assert_true(is_filename_valid( "sub/d.swu"));
	assert_false(is_filename_valid("/c.swu"));
	assert_false(is_filename_valid("../b.swu"));
	assert_false(is_filename_valid("sub/../e.swu"));
}

static void test_status_known_values(void **state)
{
	(void)state;
	assert_string_equal(get_status_string(IDLE),       "IDLE");
	assert_string_equal(get_status_string(START),      "START");
	assert_string_equal(get_status_string(RUN),        "RUN");
	assert_string_equal(get_status_string(SUCCESS),    "SUCCESS");
	assert_string_equal(get_status_string(FAILURE),    "FAILURE");
	assert_string_equal(get_status_string(DOWNLOAD),   "DOWNLOAD");
	assert_string_equal(get_status_string(DONE),       "DONE");
	assert_string_equal(get_status_string(SUBPROCESS), "SUBPROCESS");
}

static void test_status_out_of_range(void **state)
{
	(void)state;
	/* PROGRESS is intentionally absent from the string table */
	assert_string_equal(get_status_string(PROGRESS), "UNKNOWN");
	assert_string_equal(get_status_string(9999),      "UNKNOWN");
}

static void test_source_known_values(void **state)
{
	(void)state;
	assert_string_equal(get_source_string(SOURCE_UNKNOWN),    "UNKNOWN");
	assert_string_equal(get_source_string(SOURCE_WEBSERVER),  "WEBSERVER");
	assert_string_equal(get_source_string(SOURCE_SURICATTA),  "SURICATTA");
	assert_string_equal(get_source_string(SOURCE_DOWNLOADER), "DOWNLOADER");
	assert_string_equal(get_source_string(SOURCE_LOCAL),      "LOCAL");
}

static void test_source_out_of_range(void **state)
{
	(void)state;
	/* SOURCE_CHUNKS_DOWNLOADER is intentionally absent from the string table */
	assert_string_equal(get_source_string(SOURCE_CHUNKS_DOWNLOADER), "UNKNOWN");
	assert_string_equal(get_source_string(9999),                     "UNKNOWN");
}

static void test_util_substring(void **state)
{
	(void)state;
	char *s;

	s = substring("abcdef", 0, 3);
	assert_string_equal(s, "abc");
	free(s);
	s = substring("abcdef", 2, 3);
	assert_string_equal(s, "cde");
	free(s);
	/* the length is limited to the end of the string */
	s = substring("abcdef", 4, 10);
	assert_string_equal(s, "ef");
	free(s);
	s = substring("abcdef", 6, 1);
	assert_string_equal(s, "");
	free(s);
	assert_null(substring("abcdef", 7, 1));
	assert_null(substring("abcdef", -1, 1));
}

static void test_util_compare_versions(void **state)
{
	(void)state;
	/* numbering schema: up to 4 fields, missing fields are 0 */
	assert_int_equal(compare_versions("1.2.3.4", "1.2.3.4"), 0);
	assert_int_equal(compare_versions("1.2", "1.2.0.0"), 0);
	assert_int_equal(compare_versions("1.2.3", "1.2.10"), -1);
	assert_int_equal(compare_versions("2.0", "1.65535.65535.65535"), 1);
	/* fields after the 4th are not considered, see documentation */
	assert_int_equal(compare_versions("1.2.3.4", "1.2.3.4.5"), 0);
	/* semantic versions */
	assert_int_equal(compare_versions("1.2.3-rc1", "1.2.3"), -1);
	assert_int_equal(compare_versions("1.10.0+build", "1.9.0"), 1);
	/* anything else is compared as string */
	assert_true(compare_versions("abc", "abd") < 0);
}

static void test_level_known(void **state)
{
	(void)state;
	assert_int_equal(level_to_rfc_5424(ERRORLEVEL), 3);
	assert_int_equal(level_to_rfc_5424(WARNLEVEL), 4);
	assert_int_equal(level_to_rfc_5424(INFOLEVEL), 6);
	assert_int_equal(level_to_rfc_5424(TRACELEVEL), 7);
	assert_int_equal(level_to_rfc_5424(DEBUGLEVEL), 7);
}

static void test_level_unknown(void **state)
{
	(void)state;
	assert_int_equal(level_to_rfc_5424(OFF),  7);
	assert_int_equal(level_to_rfc_5424(-1),   7);
	assert_int_equal(level_to_rfc_5424(9999), 7);
}

int main(void)
{
	int error_count = 0;
	const struct CMUnitTest util_tests[] = {
	    cmocka_unit_test(test_level_known),
	    cmocka_unit_test(test_level_unknown),
	    cmocka_unit_test(test_source_known_values),
	    cmocka_unit_test(test_source_out_of_range),
	    cmocka_unit_test(test_status_known_values),
	    cmocka_unit_test(test_status_out_of_range),
	    cmocka_unit_test(test_util_ustrtoull),
	    cmocka_unit_test(test_util_size_delimiter_match),
	    cmocka_unit_test(test_util_is_filename_valid),
	    cmocka_unit_test(test_util_substring),
	    cmocka_unit_test(test_util_compare_versions)
	};
	error_count += cmocka_run_group_tests_name("util", util_tests,
						   util_setup, util_teardown);
	return error_count;
}
