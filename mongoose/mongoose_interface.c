//
// (C) Copyright 2013
// Stefano Babic, DENX Software Engineering, sbabic@denx.de.
//
// Copyright (c) 2004-2013 Sergey Lyubka
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#define _XOPEN_SOURCE 600  // For PATH_MAX on linux

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdarg.h>

#include <string.h>

#include <ctype.h>
#include <sys/wait.h>
#include <unistd.h>
#include <assert.h>

#include "mongoose.h"
#include "mongoose_interface.h"
#include "network_ipc.h"
#include "util.h"

#ifdef USE_LUA
#include <lua.h>
#include <lauxlib.h>
#endif

#define DIRSEP '/'
#define MAX_OPTIONS 40
#define MAX_CONF_FILE_LINE_SIZE (8 * 1024)
#define BUF_LEN 8192

static char server_name[40];        // Set by init_server_name()
static char config_file[PATH_MAX];  // Set by process_command_line_arguments()
static struct mg_context *ctx;      // Set by start_mongoose()

#if !defined(CONFIG_FILE)
#define CONFIG_FILE "mongoose.conf"
#endif /* !CONFIG_FILE */

static void upload_handler(struct mg_connection *conn,
		const char __attribute__ ((__unused__)) *path) {
	mg_printf(conn, "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\n"
		"\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
		"<html xmlns=\\\"http://www.w3.org/1999/xhtml\">");
	mg_printf(conn, "<head><meta http-equiv=\"refresh\" content=\"0; url=./update.html\" />"
		"</head></html>");

}

static void die(const char *fmt, ...) {
	va_list ap;
	char msg[200];

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	fprintf(stderr, "%s\n", msg);

	exit(EXIT_FAILURE);
}

static void verify_document_root(const char *root) {
	const char *p, *path;
	char buf[PATH_MAX];
	struct stat st;

	path = root;
	if ((p = strchr(root, ',')) != NULL && (size_t) (p - root) < sizeof(buf)) {
		memcpy(buf, root, p - root);
		buf[p - root] = '\0';
		path = buf;
	}

	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		die("Invalid root directory: [%s]: %s", root, strerror(errno));
	}
}

static void set_option(char **options, const char *name, const char *value) {
	int i;

	if (!strcmp(name, "document_root") || !(strcmp(name, "r"))) {
		verify_document_root(value);
	}

	for (i = 0; i < MAX_OPTIONS - 3; i++) {
		if (options[i] == NULL) {
			options[i] = sdup(name);
			options[i + 1] = sdup(value);
			options[i + 2] = NULL;
			break;
		}
	}

	if (i == MAX_OPTIONS - 3) {
		die("%s", "Too many options specified");
	}
}

static void process_command_line_arguments(int argc, char *argv[], char **options) {
	char line[MAX_CONF_FILE_LINE_SIZE], opt[sizeof(line)], val[sizeof(line)], *p;
	FILE *fp = NULL;
	size_t i, cmd_line_opts_start = 1, line_no = 0;

	options[0] = NULL;

	// Should we use a config file ?
	if (argv[1] != NULL && argv[1][0] != '-') {
		snprintf(config_file, sizeof(config_file), "%s", argv[1]);
		cmd_line_opts_start = 2;
	} else if ((p = strrchr(argv[0], DIRSEP)) == NULL) {
		// No command line flags specified. Look where binary lives
		snprintf(config_file, sizeof(config_file), "%s", CONFIG_FILE);
	} else {
		snprintf(config_file, sizeof(config_file), "%.*s%c%s",
				(int) (p - argv[0]), argv[0], DIRSEP, CONFIG_FILE);
	}

	fp = fopen(config_file, "r");

	// If config file was set in command line and open failed, die
	if (cmd_line_opts_start == 2 && fp == NULL) {
		die("Cannot open config file %s: %s", config_file, strerror(errno));
	}

	// Load config file settings first
	if (fp != NULL) {
		fprintf(stderr, "Loading config file %s\n", config_file);

		// Loop over the lines in config file
		while (fgets(line, sizeof(line), fp) != NULL) {
			line_no++;

			// Ignore empty lines and comments
			for (i = 0; isspace(* (unsigned char *) &line[i]); ) i++;
			if (line[i] == '#' || line[i] == '\0') {
				continue;
			}

			if (sscanf(line, "%s %[^\r\n#]", opt, val) != 2) {
				printf("%s: line %d is invalid, ignoring it:\n %s",
						config_file, (int) line_no, line);
			} else {
				set_option(options, opt, val);
			}
		}

		(void) fclose(fp);
	}
	// Handle command line flags.
	// They override config file and default settings.
	for (i = cmd_line_opts_start; argv[i] != NULL && i < argc - 1; i += 2) {
		if (argv[i][0] != '-' || argv[i + 1] == NULL) {
			//      show_usage_and_exit();
		}
		set_option(options, &argv[i][1], argv[i + 1]);
	}
}

static void init_server_name(void) {
	snprintf(server_name, sizeof(server_name), "Mongoose web server v. %s",
			mg_version());
}

static int log_message(const struct mg_connection *conn, const char *message) {
	(void) conn;
	printf("%s\n", message);
	return 0;
}

static int recovery_upload(struct mg_connection *conn) {
	const char *content_type_header, *boundary_start;
	char buf[BUF_LEN], path[PATH_MAX], fname[1024], boundary[100];
	int bl =  0, n, i, j, headers_len = 0, boundary_len = 0, len = 0, num_uploaded_files = 0;
	int instfd;
	int file_length;
	int nbytes = 0;
	int XHTTPRequest = 0;

	// Request looks like this:
	//
	// POST /upload HTTP/1.1
	// Host: 127.0.0.1:8080
	// Content-Length: 244894
	// Content-Type: multipart/form-data; boundary=----WebKitFormBoundaryRVr
	//
	// ------WebKitFormBoundaryRVr
	// Content-Disposition: form-data; name="file"; filename="accum.png"
	// Content-Type: image/png
	//
	//  <89>PNG
	//  <PNG DATA>
	// ------WebKitFormBoundaryRVr

	fname[0] = '\0';
	// Extract boundary string from the Content-Type header
	if ((content_type_header = mg_get_header(conn, "Content-Type")) == NULL ||
			(boundary_start = mg_strcasestr(content_type_header,
							"boundary=")) == NULL ||
			(sscanf(boundary_start, "boundary=\"%99[^\"]\"", boundary) == 0 &&
			 sscanf(boundary_start, "boundary=%99s", boundary) == 0) ||
			boundary[0] == '\0') {

		if ((content_type_header = mg_get_header(conn, "X_FILENAME")) == NULL)
			return num_uploaded_files;
		strncpy(fname, content_type_header, sizeof(fname));
		if ((content_type_header = mg_get_header(conn, "Content-length")) == NULL)
			return num_uploaded_files;
		file_length = strtoul(content_type_header, NULL, 10);
		printf("X_FILENAME: %s length: %d\n", fname, file_length);
		XHTTPRequest = 1;
	}

	/*
	 * if it is not a HTTPRequest(), get boundary to retrieve
	 * position of the file and the filename
	 */
	if (!XHTTPRequest) {
		boundary_len = strlen(boundary);
		bl = boundary_len + 4;  // \r\n--<boundary>

		// Pull in headers
		assert(len >= 0 && len <= (int) sizeof(buf));
		while ((n = mg_read(conn, buf + len, sizeof(buf) - len)) > 0) {
			len += n;
		}
		if ((headers_len = get_request_len(buf, len)) <= 0) {
			//break;
			return num_uploaded_files;
		}

		// Fetch file name.
		fname[0] = '\0';
		for (i = j = 0; i < headers_len; i++) {
			if (buf[i] == '\r' && buf[i + 1] == '\n') {
				buf[i] = buf[i + 1] = '\0';
				// TODO(lsm): don't expect filename to be the 3rd field,
				// parse the header properly instead.
				sscanf(&buf[j], "Content-Disposition: %*s %*s filename=\"%1023[^\"]",
					fname);
				j = i + 2;
			}
		}
	} else {
		while ((n = mg_read(conn, buf + len, sizeof(buf) - len)) > 0) {
			len += n;
		}
	}

	// Give up if the headers are not what we expect
	if (fname[0] == '\0') {
		//break;
		return num_uploaded_files;
	}


	instfd = ipc_inst_start();
	if (instfd < 0) {
		return num_uploaded_files;
	}

	// Move data to the beginning of the buffer
	assert(len >= headers_len);
	memmove(buf, &buf[headers_len], len - headers_len);
	len -= headers_len;

	// Read POST data, write into file until boundary is found.
	n = 0;
	nbytes = len;
	do {
		len += n;
		nbytes+=n;
		if (!XHTTPRequest) {
			for (i = 0; i < len - bl; i++) {
				if (!memcmp(&buf[i], "\r\n--", 4) &&
					!memcmp(&buf[i + 4], boundary, boundary_len)) {
					// Found boundary, that's the end of file data.

					ipc_send_data(instfd, buf, i);
					num_uploaded_files++;
					upload_handler(conn, path);

					memmove(buf, &buf[i + bl], len - (i + bl));
					len -= i + bl;
					break;
				}
			}
		} else {
			if (nbytes >= file_length) {
				ipc_send_data(instfd, buf, len);
				num_uploaded_files++;
				upload_handler(conn, path);
				break;
			}

		}
		if (len > bl) {
			ipc_send_data(instfd, buf, len - bl);
			memmove(buf, &buf[len - bl], bl);
			len = bl;
		}
	} while ((n = mg_read(conn, buf + len, sizeof(buf) - len)) > 0);
	ipc_end(instfd);

	mg_printf(conn, "%s", "HTTP/1.0 200 OK\r\n\r\n");
	return num_uploaded_files;
}

static void recovery_status(struct mg_connection *conn) {
	ipc_message ipc;
	int ret;
	char buf[4096];

	ret = ipc_get_status(&ipc);

	if (ret) {
		mg_printf(conn, "%s", "HTTP/1.0 500 Internal Server Error\r\n\r\n");
		return;
	}

	snprintf(buf, sizeof(buf),
		"{\r\n"
		"\t\"Status\" : \"%d\",\r\n"
		"\t\"Msg\" : \"%s\",\r\n"
		"\t\"Error\" : \"%d\",\r\n"
		"\t\"LastResult\" : \"%d\"\r\n"
		"}\r\n",
		ipc.data.status.current,
		strlen(ipc.data.status.desc) ? ipc.data.status.desc : "",
		ipc.data.status.error,
		ipc.data.status.last_result
		);

	mg_printf(conn,
		"HTTP/1.1 200 OK\r\n"
		"Cache: no-cache\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: %u\r\n"
		"\r\n", (unsigned int)strlen(buf));
	mg_write(conn, buf, strlen(buf));
}

static void reboot_target(struct mg_connection *conn) {
	const struct mg_request_info * reqInfo = mg_get_request_info(conn);
	int ret;

	if(!strcmp(reqInfo->request_method,"POST")) {
		mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/plain\r\n"
			"\r\n"
			"Device will reboot now.");
		ret = system("reboot");
		if (ret) {
			mg_printf(conn,
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: text/plain\r\n"
				"\r\n"
				"Device cannot be reboot, internal fault.");
		}
	}
	else {
		mg_printf(conn,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/html\r\n"
			"\r\n"
			"<form method='POST' action=''><input type='submit' value='Reboot'></form>");
	}
}

static int begin_request_handler(struct mg_connection *conn) {
	if (!strcmp(mg_get_request_info(conn)->uri, "/handle_post_request")) {
		mg_printf(conn, "%s", "HTTP/1.0 200 OK\r\n\r\n");
		recovery_upload(conn);
		return 1;
	}
	if (!strcmp(mg_get_request_info(conn)->uri, "/getstatus.json")) {
		recovery_status(conn);
		return 1;
	}
	if (!strcmp(mg_get_request_info(conn)->uri, "/rebootTarget")) {
		reboot_target(conn);
		return 1;
	}
	return 0;
}

void start_mongoose(__attribute__((unused)) int argc, char *argv[]) {
	struct mg_callbacks callbacks;
	char *options[MAX_OPTIONS];
	int i;

	init_server_name();

	/* Update config based on command line arguments */
	process_command_line_arguments(argc, argv, options);

	/* Start Mongoose */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.log_message = &log_message;

	callbacks.begin_request = begin_request_handler;
	callbacks.upload = upload_handler;


	ctx = mg_start(&callbacks, NULL, (const char **) options);
	for (i = 0; options[i] != NULL; i++) {
		free(options[i]);
	}

	if (ctx == NULL) {
		die("%s", "Failed to start Mongoose.");
	}

	printf("%s started on port(s) %s with web root [%s]\n",
			server_name, mg_get_option(ctx, "listening_ports"),
			mg_get_option(ctx, "document_root"));

}

