/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdlib.h>
#include <string.h>

#include "mongoose/mongoose.h"

void mongoose_upload_ok_reply(struct mg_connection *nc,
			      const struct mg_str *filename, size_t len);

static void test_upload_ok_reply_format(void **state)
{
	(void)state;

	struct mg_connection nc;
	const struct mg_str filename = mg_str("test.swu");
	const char *expected_body = "Ok, test.swu - 20 bytes.\r\n";
	const size_t filesize = 20;
	char *buf;
	char *body;
	const char *line;
	char *line_end;
	int line_idx = 0;

	memset(&nc, 0, sizeof(nc));

	mongoose_upload_ok_reply(&nc, &filename, filesize);

	// copy the data to a temporary buffer, to use null-terminated string functions
	assert_non_null(nc.send.buf);
	buf = malloc(nc.send.len + 1);
	assert_non_null(buf);
	memcpy(buf, nc.send.buf, nc.send.len);
	buf[nc.send.len] = '\0';

	line = buf;
	while (true) {
		line_end = strstr(line, "\r\n");
		assert_non_null(line_end);
		if (line_end == line) {
			body = line_end + 2;
			break;
		}
		*line_end = '\0';
		if (line_idx == 0)
			assert_string_equal(line, "HTTP/1.1 200 OK");
		else if (line_idx == 1)
			assert_string_equal(line, "Content-Type: text/plain");
		else if (line_idx == 2)
			assert_string_equal(line, "Connection: close");
		else if (line_idx == 3)
			assert_string_equal(line, "Content-Length: 26         ");
		else
			fail();
		line_idx++;
		line = line_end + 2;
	}
	assert_int_equal(line_idx, 4);
	assert_string_equal(body, expected_body);

	free(buf);
	mg_iobuf_free(&nc.send);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_upload_ok_reply_format),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
