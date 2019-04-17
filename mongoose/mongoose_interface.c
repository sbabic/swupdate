/*
 * Copyright (C) 2017-2018 Weidmüller Interface GmbH & Co. KG
 * Stefan Herbrechtsmeier <stefan.herbrechtsmeier@weidmueller.com>
 *
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * Copyright (c) 2004-2013 Sergey Lyubka
 *
 * SPDX-License-Identifier: MIT AND GPL-2.0-or-later
 */

#define _XOPEN_SOURCE 600  // For PATH_MAX on linux

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#include <getopt.h>

#include <network_ipc.h>
#include <mongoose_interface.h>
#include <parselib.h>
#include <progress_ipc.h>
#include <swupdate_settings.h>

#include "mongoose.h"

#define MG_PORT "8080"
#define MG_ROOT "."

enum MONGOOSE_API_VERSION {
	MONGOOSE_API_V1 = 1,
	MONGOOSE_API_V2
};

struct mongoose_options {
	char *root;
	bool listing;
	char *port;
	char *global_auth_file;
	char *auth_domain;
#if MG_ENABLE_SSL
	char *ssl_cert;
	char *ssl_key;
#endif
	enum MONGOOSE_API_VERSION api_version;
};

struct file_upload_state {
	size_t len;
	int fd;
};

static bool run_postupdate;
static struct mg_serve_http_opts s_http_server_opts;
static void upload_handler(struct mg_connection *nc, int ev, void *p);

/*
 * These functions are for V2 of the protocol
 */
#define enum_string(x)	[x] = #x
static const char *get_status_string(unsigned int status)
{
	const char * const str[] = {
		enum_string(IDLE),
		enum_string(START),
		enum_string(RUN),
		enum_string(SUCCESS),
		enum_string(FAILURE),
		enum_string(DOWNLOAD),
		enum_string(DONE),
		enum_string(SUBPROCESS)
	};

	if (status >= ARRAY_SIZE(str))
		return "UNKNOWN";

	return str[status];
}

#define enum_source_string(x)	[SOURCE_##x] = #x
static const char *get_source_string(unsigned int source)
{
	const char * const str[] = {
		enum_source_string(UNKNOWN),
		enum_source_string(WEBSERVER),
		enum_source_string(SURICATTA),
		enum_source_string(DOWNLOADER),
		enum_source_string(LOCAL)
	};

	if (source >= ARRAY_SIZE(str))
		return "UNKNOWN";

	return str[source];
}

/* Write escaped output to sized buffer */
static size_t snescape(char *dst, size_t n, const char *src)
{
	size_t len = 0;

	if (n < 3)
		return 0;

	memset(dst, 0, n);

	for (int i = 0; src[i] != '\0'; i++) {
		if (src[i] == '\\' || src[i] == '\"') {
			if (len < n - 2)
				dst[len] = '\\';
			len++;
		}
		if (len < n - 1)
			dst[len] = src[i];
		len++;
	}

	return len;
}

static void restart_handler(struct mg_connection *nc, int ev, void *ev_data)
{
	struct http_message *hm = (struct http_message *) ev_data;
	ipc_message msg = {};

	(void)ev;

	if(mg_vcasecmp(&hm->method, "POST") != 0) {
		mg_http_send_error(nc, 405, "Method Not Allowed");
		return;
	}

	int ret = ipc_postupdate(&msg);
	if (ret) {
		mg_http_send_error(nc, 500, "Failed to queue command");
		return;
	}

	mg_http_send_error(nc, 201, "Device will reboot now.");
}

static void broadcast_callback(struct mg_connection *nc, int ev, void *ev_data)
{
	char *buf = (char *) ev_data;

	if (ev != MG_EV_POLL)
		return;

	if (!(nc->flags & MG_F_IS_WEBSOCKET))
		return;

	mg_send_websocket_frame(nc, WEBSOCKET_OP_TEXT, buf, strlen(buf));
}

static void broadcast(struct mg_mgr *mgr, char *str)
{
	mg_broadcast(mgr, broadcast_callback, str, strlen(str) + 1);
}

static void *broadcast_message_thread(void *data)
{
	for (;;) {
		ipc_message msg;
		int ret = ipc_get_status(&msg);

		if (!ret && strlen(msg.data.status.desc) != 0) {
			struct mg_mgr *mgr = (struct mg_mgr *) data;
			char text[4096];
			char str[4160];

			snescape(text, sizeof(text), msg.data.status.desc);

			snprintf(str, sizeof(str),
				"{\r\n"
				"\t\"type\": \"message\",\r\n"
				"\t\"level\": \"%d\",\r\n"
				"\t\"text\": \"%s\"\r\n"
				"}\r\n",
				(msg.data.status.error) ? 3 : 6, /* RFC 5424 */
				text);

			broadcast(mgr, str);
			continue;
		}

		usleep(50 * 1000);
	}

	return NULL;
}

static void *broadcast_progress_thread(void *data)
{
	RECOVERY_STATUS status = -1;
	sourcetype source = -1;
	unsigned int step = 0;
	unsigned int percent = 0;
	int fd = -1;

	for (;;) {
		struct mg_mgr *mgr = (struct mg_mgr *) data;
		struct progress_msg msg;
		char str[512];
		int ret;

		if (fd < 0)
			fd = progress_ipc_connect(true);

		ret = progress_ipc_receive(&fd, &msg);
		if (ret != sizeof(msg))
			return NULL;

		if (msg.status != status || msg.status == FAILURE) {
			status = msg.status;

			snprintf(str, sizeof(str),
				"{\r\n"
				"\t\"type\": \"status\",\r\n"
				"\t\"status\": \"%s\"\r\n"
				"}\r\n",
				get_status_string(msg.status));
			broadcast(mgr, str);
		}

		if (msg.source != source) {
			source = msg.source;

			snprintf(str, sizeof(str),
				"{\r\n"
				"\t\"type\": \"source\",\r\n"
				"\t\"source\": \"%s\"\r\n"
				"}\r\n",
				get_source_string(msg.source));
			broadcast(mgr, str);
		}

		if (msg.status == SUCCESS && msg.source == SOURCE_WEBSERVER && run_postupdate) {
			ipc_message ipc = {};

			ipc_postupdate(&ipc);
		}

		if (msg.infolen) {
			snprintf(str, sizeof(str),
				"{\r\n"
				"\t\"type\": \"info\",\r\n"
				"\t\"source\": \"%s\"\r\n"
				"}\r\n",
				msg.info);
			broadcast(mgr, str);
		}

		if ((msg.cur_step != step || msg.cur_percent != percent) &&
				msg.cur_step) {
			step = msg.cur_step;
			percent = msg.cur_percent;

			snprintf(str, sizeof(str),
				"{\r\n"
				"\t\"type\": \"step\",\r\n"
				"\t\"number\": \"%d\",\r\n"
				"\t\"step\": \"%d\",\r\n"
				"\t\"name\": \"%s\",\r\n"
				"\t\"percent\": \"%d\"\r\n"
				"}\r\n",
				msg.nsteps,
				msg.cur_step,
				msg.cur_step ? msg.cur_image: "",
				msg.cur_percent);
			broadcast(mgr, str);
		}
	}

	return NULL;
}

/*
 * These functions are for V1 of the protocol
 */
static void upload_handler_v1(struct mg_connection *nc, int ev, void *p)
{
	struct mg_str *filename, *data;
	struct http_message *hm;
	size_t length;
	char buf[16];
	int fd;

	switch (ev) {
	case MG_EV_HTTP_REQUEST:
		hm = (struct http_message *) p;

		filename = mg_get_http_header(hm, "X_FILENAME");
		if (filename == NULL) {
			mg_http_send_error(nc, 403, NULL);
			return;
		}

		data = mg_get_http_header(hm, "Content-length");
		if (data == NULL || data->len >= ARRAY_SIZE(buf)) {
			mg_http_send_error(nc, 403, NULL);
			return;
		}

		memcpy(buf, data->p, data->len);
		buf[data->len] = '\0';
		length = strtoul(data->p, NULL, 10);
		if (length == 0) {
			mg_http_send_error(nc, 403, NULL);
			return;
		}

		fd = ipc_inst_start_ext(SOURCE_WEBSERVER, filename->len, filename->p, false);
		ipc_send_data(fd, (char *) hm->body.p, hm->body.len);
		ipc_end(fd);

		mg_send_response_line(nc, 200,
			"Content-Type: text/plain\r\n"
			"Connection: close");
		mg_send(nc, "\r\n", 2);
		mg_printf(nc, "Ok, %.*s - %d bytes.\r\n", (int) filename->len, filename->p, (int) length);
		nc->flags |= MG_F_SEND_AND_CLOSE;
		break;
	default:
		upload_handler(nc, ev, p);
		break;
	}
}

static void recovery_status(struct mg_connection *nc, int ev, void *ev_data)
{
	ipc_message ipc;
	int ret;
	char buf[4096];

	(void)ev;
	(void)ev_data;

	ret = ipc_get_status(&ipc);

	if (ret) {
		mg_http_send_error(nc, 500, NULL);
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
		ipc.data.status.last_result);

	mg_send_head(nc, 200, strlen(buf),
		"Cache: no-cache\r\n"
		"Content-Type: text/plain");

	mg_send(nc, buf, strlen(buf));

	nc->flags |= MG_F_SEND_AND_CLOSE;
}

/*
 * Code common to V1 and V2
 */
static void upload_handler(struct mg_connection *nc, int ev, void *p)
{
	struct mg_http_multipart_part *mp;
	struct file_upload_state *fus;

	switch (ev) {
	case MG_EV_HTTP_PART_BEGIN:
		mp = (struct mg_http_multipart_part *) p;

		fus = (struct file_upload_state *) calloc(1, sizeof(*fus));
		if (fus == NULL) {
			mg_http_send_error(nc, 500, "Out of memory");
			break;
		}

		fus->fd = ipc_inst_start_ext(SOURCE_WEBSERVER, strlen(mp->file_name), mp->file_name, false);
		if (fus->fd < 0) {
			mg_http_send_error(nc, 500, "Failed to queue command");
			free(fus);
			break;
		}

		mp->user_data = fus;

		break;

	case MG_EV_HTTP_PART_DATA:
		mp = (struct mg_http_multipart_part *) p;
		fus = (struct file_upload_state *) mp->user_data;

		if (!fus)
			break;

		ipc_send_data(fus->fd, (char *) mp->data.p, mp->data.len);
		fus->len += mp->data.len;

		break;

	case MG_EV_HTTP_PART_END:
		mp = (struct mg_http_multipart_part *) p;
		fus = (struct file_upload_state *) mp->user_data;

		if (!fus)
			break;

		ipc_end(fus->fd);

		mg_send_response_line(nc, 200,
			"Content-Type: text/plain\r\n"
			"Connection: close");
		mg_send(nc, "\r\n", 2);
		mg_printf(nc, "Ok, %s - %d bytes.\r\n", mp->file_name, (int) fus->len);
		nc->flags |= MG_F_SEND_AND_CLOSE;

		mp->user_data = NULL;
		free(fus);
		break;
	}
}

static void ev_handler_v1(struct mg_connection __attribute__ ((__unused__)) *nc,
				int __attribute__ ((__unused__)) ev,
				void __attribute__ ((__unused__)) *ev_data)
{
}

static void ev_handler(struct mg_connection *nc, int ev, void *ev_data)
{
	if (ev == MG_EV_HTTP_REQUEST) {
		mg_serve_http(nc, ev_data, s_http_server_opts);
	}
}

static int mongoose_settings(void *elem, void  __attribute__ ((__unused__)) *data)
{
	struct mongoose_options *opts = (struct mongoose_options *)data;
	char tmp[128];

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "document_root", tmp);
	if (strlen(tmp)) {
		opts->root = strdup(tmp);
	}

	get_field(LIBCFG_PARSER, elem, "enable_directory_listing",
		  &opts->listing);

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "listening_ports", tmp);
	if (strlen(tmp)) {
		opts->port = strdup(tmp);
	}
#if MG_ENABLE_SSL
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "ssl_certificate", tmp);
	if (strlen(tmp)) {
		opts->ssl_cert = strdup(tmp);
	}

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "ssl_certificate_key", tmp);
	if (strlen(tmp)) {
		opts->ssl_key = strdup(tmp);
	}
#endif
	/*
	 * Get API Version
	 */
	get_field(LIBCFG_PARSER, elem, "api", &opts->api_version);
	switch(opts->api_version) {
	case MONGOOSE_API_V1:
	case MONGOOSE_API_V2:
		break;
	default:
		opts->api_version = MONGOOSE_API_V2;
	}

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "global-auth-file", tmp);
	if (strlen(tmp)) {
		opts->global_auth_file = strdup(tmp);
	}
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "auth-domain", tmp);
	if (strlen(tmp)) {
		opts->auth_domain = strdup(tmp);
	}
	get_field(LIBCFG_PARSER, elem, "run-postupdate", &run_postupdate);

	return 0;
}


static struct option long_options[] = {
	{"listing", no_argument, NULL, 'l'},
	{"port", required_argument, NULL, 'p'},
#if MG_ENABLE_SSL
	{"ssl", no_argument, NULL, 's'},
	{"ssl-cert", required_argument, NULL, 'C'},
	{"ssl-key", required_argument, NULL, 'K'},
#endif
	{"document-root", required_argument, NULL, 'r'},
	{"api-version", required_argument, NULL, 'a'},
	{"auth-domain", required_argument, NULL, '0'},
	{"global-auth-file", required_argument, NULL, '1'},
	{NULL, 0, NULL, 0}
};

void mongoose_print_help(void)
{
	fprintf(
		stdout,
		"\tmongoose arguments:\n"
		"\t  -l, --listing                  : enable directory listing\n"
		"\t  -p, --port <port>              : server port number  (default: %s)\n"
#if MG_ENABLE_SSL
		"\t  -s, --ssl                      : enable ssl support\n"
		"\t  -C, --ssl-cert <cert>          : ssl certificate to present to clients\n"
		"\t  -K, --ssl-key <key>            : key corresponding to the ssl certificate\n"
#endif
		"\t  -r, --document-root <path>     : path to document root directory (default: %s)\n"
		"\t  -a, --api-version [1|2]        : set Web protocol API to v1 (legacy) or v2 (default v2)\n"
		"\t  --auth-domain                  : set authentication domain if any (default: none)\n"
		"\t  --global-auth-file             : set authentication file if any (default: none)\n",
		MG_PORT, MG_ROOT);
}

int start_mongoose(const char *cfgfname, int argc, char *argv[])
{
	struct mongoose_options opts;
	struct mg_mgr mgr;
	struct mg_connection *nc;
	struct mg_bind_opts bind_opts;
	const char *s_http_port = NULL;
	const char *err_str;
#if MG_ENABLE_SSL
	bool ssl = false;
#endif
	int choice = 0;

	memset(&opts, 0, sizeof(opts));
	/* Set default API version */
	opts.api_version = MONGOOSE_API_V2;

	/* No listing directory as default */
	opts.listing = false;

	/*
	 * Default value is active
	 */
	run_postupdate = true;

	if (cfgfname) {
		read_module_settings(cfgfname, "webserver", mongoose_settings, &opts);
	}

	optind = 1;
	while ((choice = getopt_long(argc, argv, "lp:sC:K:r:a:",
				     long_options, NULL)) != -1) {
		switch (choice) {
		case '0':
			free(opts.auth_domain);
			opts.auth_domain = strdup(optarg);
			break;
		case '1':
			free(opts.global_auth_file);
			opts.global_auth_file = strdup(optarg);
			break;
		case 'l':
			opts.listing = true;
			break;
		case 'p':
			free(opts.port);
			opts.port = strdup(optarg);
			break;
#if MG_ENABLE_SSL
		case 's':
			ssl = true;
			break;
		case 'C':
			free(opts.ssl_cert);
			opts.ssl_cert = strdup(optarg);
			break;
		case 'K':
			free(opts.ssl_key);
			opts.ssl_key = strdup(optarg);
			break;
#endif
		case 'r':
			free(opts.root);
			opts.root = strdup(optarg);
			break;
		case 'a':
			opts.api_version = (!strcmp(optarg, "1")) ?
						MONGOOSE_API_V1 :
						MONGOOSE_API_V2;
			break;
		case '?':
		default:
			return -EINVAL;
		}
	}

	s_http_server_opts.document_root =
		opts.root ? opts.root : MG_ROOT;
	s_http_server_opts.enable_directory_listing =
		opts.listing ? "yes" : "no";
	s_http_port = opts.port ? opts.port : MG_PORT;
	s_http_server_opts.global_auth_file = opts.global_auth_file;
	s_http_server_opts.auth_domain = opts.auth_domain;

	memset(&bind_opts, 0, sizeof(bind_opts));
	bind_opts.error_string = &err_str;
#if MG_ENABLE_SSL
	if (ssl) {
		bind_opts.ssl_cert = opts.ssl_cert;
		bind_opts.ssl_key = opts.ssl_key;
	}
#endif

	mg_mgr_init(&mgr, NULL);

	if (opts.api_version == MONGOOSE_API_V1)
		nc = mg_bind_opt(&mgr, s_http_port, ev_handler_v1, bind_opts);
	else
		nc = mg_bind_opt(&mgr, s_http_port, ev_handler, bind_opts);
	if (nc == NULL) {
		fprintf(stderr, "Failed to start Mongoose: %s\n", *bind_opts.error_string);
		exit(EXIT_FAILURE);
	}

	mg_set_protocol_http_websocket(nc);
	switch (opts.api_version) {
	case MONGOOSE_API_V1:
		mg_register_http_endpoint(nc, "/handle_post_request", MG_CB(upload_handler_v1, NULL));
		mg_register_http_endpoint(nc, "/getstatus.json", MG_CB(recovery_status, NULL));
		break;
	case MONGOOSE_API_V2:
		mg_register_http_endpoint(nc, "/restart", restart_handler);
		mg_register_http_endpoint(nc, "/upload", MG_CB(upload_handler, NULL));
		mg_start_thread(broadcast_message_thread, &mgr);
		mg_start_thread(broadcast_progress_thread, &mgr);
		break;
	}

	printf("Mongoose web server version %s with pid %d started on port(s) %s with web root [%s] and API %s\n",
		MG_VERSION, getpid(), s_http_port,
		s_http_server_opts.document_root,
		(opts.api_version  == MONGOOSE_API_V1) ? "v1" : "v2");

	for (;;) {
		mg_mgr_poll(&mgr, 100);
	}
	mg_mgr_free(&mgr);

	return 0;
}
