// SPDX-FileCopyrightText: 2026 Aviv Daum <aviv.daum@gmail.com>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "network_ipc.h"

static ipc_message last_msg;

extern int __real_ipc_send_cmd(ipc_message *msg);
int __wrap_ipc_send_cmd(ipc_message *msg);
int __wrap_ipc_send_cmd(ipc_message *msg)
{
	last_msg = *msg;
	return 0;
}

static int network_ipc_if_setup(void **state)
{
	(void)state;
	memset(&last_msg, 0, sizeof(last_msg));
	return 0;
}

static void test_swupdate_dwl_url_populates_message(void **state)
{
	(void)state;

	assert_int_equal(swupdate_dwl_url("artifact.swu", "https://example.invalid/a.swu"), 0);
	assert_int_equal(last_msg.magic, IPC_MAGIC);
	assert_int_equal(last_msg.type, SET_DELTA_URL);
	assert_string_equal(last_msg.data.dwl_url.filename, "artifact.swu");
	assert_string_equal(last_msg.data.dwl_url.url, "https://example.invalid/a.swu");
}

static void test_swupdate_dwl_url_truncates_and_terminates(void **state)
{
	char filename[400];
	char url[1200];

	(void)state;

	memset(filename, 'f', sizeof(filename) - 1);
	filename[sizeof(filename) - 1] = '\0';
	memset(url, 'u', sizeof(url) - 1);
	url[sizeof(url) - 1] = '\0';

	assert_int_equal(swupdate_dwl_url(filename, url), 0);
	assert_int_equal(last_msg.data.dwl_url.filename[sizeof(last_msg.data.dwl_url.filename) - 1], '\0');
	assert_int_equal(last_msg.data.dwl_url.url[sizeof(last_msg.data.dwl_url.url) - 1], '\0');
	assert_int_equal(strlen(last_msg.data.dwl_url.filename), sizeof(last_msg.data.dwl_url.filename) - 1);
	assert_int_equal(strlen(last_msg.data.dwl_url.url), sizeof(last_msg.data.dwl_url.url) - 1);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_swupdate_dwl_url_populates_message),
		cmocka_unit_test(test_swupdate_dwl_url_truncates_and_terminates),
	};

	return cmocka_run_group_tests_name("network_ipc_if", tests,
					   network_ipc_if_setup, NULL);
}
