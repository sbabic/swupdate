/*
 * Copyright (C) 2017-2018 Weidmüller Interface GmbH & Co. KG
 * Stefan Herbrechtsmeier <stefan.herbrechtsmeier@weidmueller.com>
 *
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * Copyright (c) 2004-2013 Sergey Lyubka
 *
 * SPDX-License-Identifier: MIT AND GPL-2.0-only
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
#include <time.h>

#include "mongoose.h"
#include "util.h"

#define MG_PORT "8080"
#define MG_ROOT "."

/* in seconds. If no packet is received with this timeout, connection is broken */
#define MG_TIMEOUT	120

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
};

struct file_upload_state {
	size_t len;
	int fd;
	bool error_report; /* if set, stop to flood with errors */
};

static bool run_postupdate;
static unsigned int watchdog_conn = 0;
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

static void restart_handler(struct mg_connection *nc, int ev, void *ev_data)
{
	struct http_message *hm = (struct http_message *) ev_data;
	ipc_message msg = {};

	if (ev == MG_EV_HTTP_REQUEST) {
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
		/*
		 * if still fails, try later
		 */
		if (fd < 0) {
			sleep(1);
			continue;
		}

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
 * Code common to V1 and V2
 */
static void upload_handler(struct mg_connection *nc, int ev, void *p)
{
	struct mg_http_multipart_part *mp;
	struct file_upload_state *fus;
	ssize_t written;

	switch (ev) {
	case MG_EV_HTTP_PART_BEGIN:
		mp = (struct mg_http_multipart_part *) p;

		fus = (struct file_upload_state *) calloc(1, sizeof(*fus));
		if (fus == NULL) {
			mg_http_send_error(nc, 500, "Out of memory");
			break;
		}

		struct swupdate_request req;
		swupdate_prepare_req(&req);
		req.len = strlen(mp->file_name);
		strncpy(req.info, mp->file_name, sizeof(req.info) - 1);
		req.source = SOURCE_WEBSERVER;
		fus->fd = ipc_inst_start_ext(&req, sizeof(req));
		if (fus->fd < 0) {
			mg_http_send_error(nc, 500, "Failed to queue command");
			free(fus);
			break;
		}

		if (swupdate_file_setnonblock(fus->fd, true)) {
			WARN("IPC cannot be set in non-blocking, fallback to block mode");
		}

		mp->user_data = fus;

		/*
		 * There is no user data for connection.
		 * Set the user data to the same structure to make it available
		 * to the MG_TIMER event
		 */
		nc->user_data = mp->user_data;

		if (watchdog_conn > 0) {
			TRACE("Setting Webserver Watchdog Timer to %d", watchdog_conn);
			mg_set_timer(nc, mg_time() + watchdog_conn);
		}

		break;

	case MG_EV_HTTP_PART_DATA:
		mp = (struct mg_http_multipart_part *) p;
		fus = (struct file_upload_state *) mp->user_data;

		if (!fus)
			break;

		written = write(fus->fd, (char *) mp->data.p, mp->data.len);
		/*
		 * IPC seems to block, wait for a while
		 */
		if (written != mp->data.len) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				if (!fus->error_report) {
					ERROR("Writing to IPC fails due to %s", strerror(errno));
					fus->error_report = true;
				}
			}
			usleep(100);

			if (written < 0)
				written = 0;
		}

		mp->num_data_consumed = written;
		fus->len += written;

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
		nc->user_data = mp->user_data;
		free(fus);
		break;
	}
}

static void ev_handler(struct mg_connection *nc, int ev, void *ev_data)
{
	time_t now;

	switch (ev) {
	case MG_EV_HTTP_REQUEST:
		mg_serve_http(nc, ev_data, s_http_server_opts);
		break;
	case MG_EV_TIMER:
		now = (time_t) mg_time();
		/*
		 * Check if a multi-part was initiated
		 */
		if (nc->user_data && (watchdog_conn > 0) &&
			(difftime(now, nc->last_io_time) > watchdog_conn)) {
			struct file_upload_state *fus;

		       /* Connection lost, drop data */
			ERROR("Connection lost, no data since %ld now %ld, closing...",
				nc->last_io_time, now);
			fus = (struct file_upload_state *) nc->user_data;
			ipc_end(fus->fd);
			nc->user_data = NULL;
			nc->flags |= MG_F_CLOSE_IMMEDIATELY;
		} else
			mg_set_timer(nc, mg_time() + watchdog_conn);  // Send us timer event again after 0.5 seconds
		break;
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

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "global-auth-file", tmp);
	if (strlen(tmp)) {
		opts->global_auth_file = strdup(tmp);
	}
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "auth-domain", tmp);
	if (strlen(tmp)) {
		opts->auth_domain = strdup(tmp);
	}
	get_field(LIBCFG_PARSER, elem, "run-postupdate", &run_postupdate);

	get_field(LIBCFG_PARSER, elem, "timeout", &watchdog_conn);

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
	{"timeout", required_argument, NULL, 't'},
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
		"\t  -t, --timeout                  : timeout to check if connection is lost (default: check disabled)\n"
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

	/* No listing directory as default */
	opts.listing = false;

	/*
	 * Default value is active
	 */
	run_postupdate = true;

	/*
	 * Default no monitor of connection
	 */
	watchdog_conn = 0;

	if (cfgfname) {
		swupdate_cfg_handle handle;
		swupdate_cfg_init(&handle);
		if (swupdate_cfg_read_file(&handle, cfgfname) == 0) {
			read_module_settings(&handle, "webserver", mongoose_settings, &opts);
		}
		swupdate_cfg_destroy(&handle);
	}

	optind = 1;
	while ((choice = getopt_long(argc, argv, "lp:sC:K:r:a:t:",
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
		case 't':
			watchdog_conn = strtoul(optarg, NULL, 10);
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

	nc = mg_bind_opt(&mgr, s_http_port, ev_handler, bind_opts);
	if (nc == NULL) {
		ERROR("Failed to start Mongoose: %s", *bind_opts.error_string);
		exit(EXIT_FAILURE);
	}

	/*
	 * The Event Handler in Webserver will read from socket until there is data.
	 * This does not guarantes a flow control because data are forwarded
	 * to SWUpdate internal IPC. If this is not called in blocking mode,
	 * the Webserver should just read from socket to fill the IPC, but without
	 * filling all memory.
	 */
	nc->recv_mbuf_limit = 256 * 1024;

	mg_set_protocol_http_websocket(nc);
	mg_register_http_endpoint(nc, "/restart", restart_handler);
	mg_register_http_endpoint(nc, "/upload", MG_CB(upload_handler, NULL));
	mg_start_thread(broadcast_message_thread, &mgr);
	mg_start_thread(broadcast_progress_thread, &mgr);

	INFO("Mongoose web server version %s with pid %d started on port(s) %s with web root [%s]",
		MG_VERSION, getpid(), s_http_port,
		s_http_server_opts.document_root);

	for (;;) {
		mg_mgr_poll(&mgr, 100);
	}
	mg_mgr_free(&mgr);

	return 0;
}

#if MG_ENABLE_SSL && MG_SSL_IF == MG_SSL_IF_MBEDTLS
#include <mbedtls/ctr_drbg.h>
int mg_ssl_if_mbed_random(void *ctx, unsigned char *buf, size_t len) {
	return mbedtls_ctr_drbg_random(ctx, buf, len);
}
#endif
