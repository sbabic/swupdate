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

#include <getopt.h>

#include <network_ipc.h>
#include <mongoose_interface.h>
#include <parselib.h>
#include <swupdate_settings.h>

#include "mongoose.h"

#define MG_LISTING "no"
#define MG_PORT "8080"
#define MG_ROOT "."

struct mongoose_options {
	char *root;
	char *listing;
	char *port;
#if MG_ENABLE_SSL
	char *ssl_cert;
	char *ssl_key;
#endif
};

struct file_upload_state {
	size_t len;
	int fd;
};

static struct mg_serve_http_opts s_http_server_opts;

static void upload_handler(struct mg_connection *nc, int ev, void *p)
{
	struct mg_http_multipart_part *mp;
	struct file_upload_state *fus;
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

		fd = ipc_inst_start_ext(SOURCE_WEBSERVER, filename->len, filename->p);
		ipc_send_data(fd, (char *) hm->body.p, hm->body.len);
		ipc_end(fd);

		mg_send_response_line(nc, 200,
			"Content-Type: text/plain\r\n"
			"Connection: close");
		mg_send(nc, "\r\n", 2);
		mg_printf(nc, "Ok, %.*s - %d bytes.\r\n", (int) filename->len, filename->p, (int) length);
		nc->flags |= MG_F_SEND_AND_CLOSE;

		break;
	case MG_EV_HTTP_PART_BEGIN:
		mp = (struct mg_http_multipart_part *) p;

		fus = (struct file_upload_state *) calloc(1, sizeof(*fus));
		if (fus == NULL) {
			mg_http_send_error(nc, 500, "Out of memory");
			break;
		}

		fus->fd = ipc_inst_start_ext(SOURCE_WEBSERVER, strlen(mp->file_name), mp->file_name);
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

static void reboot_target(struct mg_connection *nc, int ev, void *ev_data)
{
	struct http_message *hm = (struct http_message *) ev_data;
	int ret;

	(void)ev;

	if(mg_vcasecmp(&hm->method, "POST") == 0) {
		ret = system("reboot");
		if (ret) {
			mg_http_send_error(nc, 500,
				"Device cannot be reboot, internal fault.");
			return;
		}

		mg_http_send_error(nc, 200, "Device will reboot now.");
	}
	else {
		mg_send_response_line(nc, 200,
			"Content-Type: text/html\r\n"
			"Connection: close");
		mg_send(nc, "\r\n", 2);
		mg_printf(nc,
			"<form method='POST' action=''>"
			"<input type='submit' value='Reboot'>"
			"</form>");
		nc->flags |= MG_F_SEND_AND_CLOSE;
	}
}

static void post_update_cmd(struct mg_connection *nc, int ev, void *ev_data)
{
	ipc_message msg = {};

	(void)ev;
	(void)ev_data;

	int ret = ipc_postupdate(&msg);
	mg_send_response_line(nc, 200, "Content-Type: application/json");
	mg_send(nc, "\r\n", 2);
	mg_printf(nc,
		"{\r\n"
		"\t\"code\": %d,\r\n"
		"\t\"error\": \"%s\",\r\n"
		"\t\"detail\": \"%s\"\r\n"
		"}",
		(ret == 0) ? 200 : 501,
		(ret == 0) ? "" : "Internal server error",
		(ret == 0) ? "" : "Failed to queue command");

	nc->flags |= MG_F_SEND_AND_CLOSE;
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

	GET_FIELD_STRING(LIBCFG_PARSER, elem, "document_root", tmp);
	if (strlen(tmp)) {
		opts->root = strdup(tmp);
	}

	GET_FIELD_STRING(LIBCFG_PARSER, elem, "enable_directory_listing", tmp);
	if (strlen(tmp)) {
		opts->listing = strdup(tmp);
	}

	GET_FIELD_STRING(LIBCFG_PARSER, elem, "listening_ports", tmp);
	if (strlen(tmp)) {
		opts->port = strdup(tmp);
	}
#if MG_ENABLE_SSL
	GET_FIELD_STRING(LIBCFG_PARSER, elem, "ssl_certificate", tmp);
	if (strlen(tmp)) {
		opts->ssl_cert = strdup(tmp);
	}

	GET_FIELD_STRING(LIBCFG_PARSER, elem, "ssl_certificate_key", tmp);
	if (strlen(tmp)) {
		opts->ssl_key = strdup(tmp);
	}
#endif
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
	{NULL, 0, NULL, 0}
};

void mongoose_print_help(void)
{
	fprintf(
		stdout,
		"\tmongoose arguments:\n"
		"\t  -l, --listing <port>           : enable directory listing  (default: %s)\n"
		"\t  -p, --port <port>              : server port number  (default: %s)\n"
#if MG_ENABLE_SSL
		"\t  -s, --ssl                      : enable ssl support\n"
		"\t  -C, --ssl-cert <cert>          : ssl certificate to present to clients\n"
		"\t  -K, --ssl-key <key>            : key corresponding to the ssl certificate\n"
#endif
		"\t  -r, --document-root <path>     : path to document root directory (default: %s)\n",
		MG_LISTING, MG_PORT, MG_ROOT);
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
	if (cfgfname) {
		read_module_settings(cfgfname, "webserver", mongoose_settings, &opts);
	}

	optind = 1;
	while ((choice = getopt_long(argc, argv, "lp:sC:K:r:",
				     long_options, NULL)) != -1) {
		switch (choice) {
		case 'l':
			free(opts.listing);
			opts.listing = strdup(optarg);
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
		case '?':
		default:
			return -EINVAL;
		}
	}

	s_http_server_opts.document_root =
		opts.root ? opts.root : MG_ROOT;
	s_http_server_opts.enable_directory_listing =
		opts.listing ? opts.listing : MG_LISTING;
	s_http_port = opts.port ? opts.port : MG_PORT;

	memset(&bind_opts, 0, sizeof(bind_opts));
	bind_opts.error_string = &err_str;
#if MG_ENABLE_SSL
	if (ssl) {
		bind_opts.ssl_cert = opts.ssl_cert;
		bind_opts.ssl_key = opts.ssl_key;
	}
#endif

	mg_mgr_init(&mgr, NULL);

	nc = mg_bind_opt(&mgr, s_http_port, ev_handler, bind_opts);
	if (nc == NULL) {
		fprintf(stderr, "Failed to start Mongoose: %s\n", *bind_opts.error_string);
		exit(EXIT_FAILURE);
	}

	mg_register_http_endpoint(nc, "/handle_post_request", MG_CB(upload_handler, NULL));
	mg_register_http_endpoint(nc, "/getstatus.json", MG_CB(recovery_status, NULL));
	mg_register_http_endpoint(nc, "/rebootTarget", MG_CB(reboot_target, NULL));
	mg_register_http_endpoint(nc, "/postUpdateCommand", MG_CB(post_update_cmd, NULL));
	mg_register_http_endpoint(nc, "/upload", MG_CB(upload_handler, NULL));
	mg_set_protocol_http_websocket(nc);

	printf("Mongoose web server version %s with pid %d started on port(s) %s with web root [%s]\n",
		MG_VERSION, getpid(), s_http_port,
		s_http_server_opts.document_root);

	for (;;) {
		mg_mgr_poll(&mgr, 100);
	}
	mg_mgr_free(&mgr);

	return 0;
}
