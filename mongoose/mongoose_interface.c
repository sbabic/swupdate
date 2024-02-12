/*
 * Copyright (C) 2017-2018 Weidmüller Interface GmbH & Co. KG
 * Stefan Herbrechtsmeier <stefan.herbrechtsmeier@weidmueller.com>
 *
 * (C) Copyright 2013
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * Copyright (c) 2004-2013 Sergey Lyubka
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "swupdate_status.h"
#define _XOPEN_SOURCE 600  // For PATH_MAX on linux

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdbool.h>

#include <getopt.h>

#include <network_ipc.h>
#include <mongoose_interface.h>
#include <parselib.h>
#include <progress_ipc.h>
#include <swupdate_settings.h>
#include <pctl.h>
#include <progress.h>

#include "mongoose.h"
#include "mongoose_multipart.h"
#include "util.h"

#ifndef MG_ENABLE_SSL
#define MG_ENABLE_SSL 0
#endif

#define MG_PORT "8080"
#define MG_ROOT "."

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
	struct mg_connection *c;
	size_t len;
	int fd;
	bool error_report; /* if set, stop to flood with errors */
	uint8_t percent;
	struct mg_timer *timer;
	uint64_t last_io_time;
};

static bool run_postupdate;
static unsigned int watchdog_conn = 0;
static struct mg_http_serve_opts s_http_server_opts;
const char *global_auth_domain;
const char *global_auth_file;
#if MG_ENABLE_SSL
static bool ssl;
static struct mg_tls_opts tls_opts;
#endif

static int ws_pipe;

static int s_signo = 0;
static void signal_handler(int signo) {
	s_signo = signo;
}

static int p_stat(const char *path, size_t *size, time_t *mtime)
{
	int flags = mg_fs_posix.st(path, size, mtime);
	if (flags & MG_FS_DIR && strcmp(s_http_server_opts.root_dir, path) != 0)
		return 0;
	return flags;
}

static void p_list(const char *path, void (*fn)(const char *, void *), void *userdata)
{
	(void) path, (void) fn, (void) userdata;
}

static void *p_open(const char *path, int flags)
{
	return mg_fs_posix.op(path, flags);
}

static void p_close(void *fp)
{
	return mg_fs_posix.cl(fp);
}

static size_t p_read(void *fd, void *buf, size_t len)
{
	return mg_fs_posix.rd(fd, buf, len);
}

static size_t p_write(void *fd, const void *buf, size_t len)
{
	return mg_fs_posix.wr(fd, buf, len);
}

static size_t p_seek(void *fd, size_t offset)
{
	return mg_fs_posix.sk(fd, offset);
}

static bool p_rename(const char *from, const char *to)
{
	return mg_fs_posix.mv(from, to);
}

static bool p_remove(const char *path)
{
	return mg_fs_posix.rm(path);
}

static bool p_mkdir(const char *path)
{
	return mg_fs_posix.mkd(path);
}

/* mg_fs which inhibits directory listing functionality */
static struct mg_fs fs_posix_no_list = {
		p_stat,
		p_list,
		p_open,
		p_close,
		p_read,
		p_write,
		p_seek,
		p_rename,
		p_remove,
		p_mkdir
};

/*
 * Minimal forward port of mongoose digest auth support.
 */
static void mg_hash_md5_v(size_t num_msgs, const uint8_t *msgs[],
				   const size_t *msg_lens, uint8_t *digest)
{
	size_t i;
	mg_md5_ctx md5_ctx;
	mg_md5_init(&md5_ctx);
	for (i = 0; i < num_msgs; i++) {
		mg_md5_update(&md5_ctx, msgs[i], msg_lens[i]);
	}
	mg_md5_final(&md5_ctx, digest);
}

static void cs_md5(char buf[33], ...)
{
	unsigned char hash[16];
	const uint8_t *msgs[20], *p;
	size_t msg_lens[20];
	size_t num_msgs = 0;
	va_list ap;

	va_start(ap, buf);
	while ((p = va_arg(ap, const unsigned char *)) != NULL) {
		msgs[num_msgs] = p;
		msg_lens[num_msgs] = va_arg(ap, size_t);
		num_msgs++;
	}
	va_end(ap);

	mg_hash_md5_v(num_msgs, msgs, msg_lens, hash);
	mg_hex(hash, sizeof(hash), buf);
}

static void mg_mkmd5resp(struct mg_str method, struct mg_str uri, struct mg_str ha1,
						 struct mg_str nonce, struct mg_str nc, struct mg_str cnonce,
						 struct mg_str qop, char *resp)
{
	static const char colon[] = ":";
	static const size_t one = 1;
	char ha2[33];
	cs_md5(ha2, method.ptr, method.len, colon, one, uri.ptr, uri.len, NULL);
	cs_md5(resp, ha1.ptr, ha1.len, colon, one, nonce.ptr, nonce.len, colon, one, nc.ptr,
		   nc.len, colon, one, cnonce.ptr, cnonce.len, colon, one, qop.ptr, qop.len,
		   colon, one, ha2, sizeof(ha2) - 1, NULL);
}

static double mg_time(void)
{
	struct timeval tv;
	if (gettimeofday(&tv, NULL /* tz */) != 0) return 0;
	return (double) tv.tv_sec + (((double) tv.tv_usec) / 1000000.0);
}

/*
 * Check for authentication timeout.
 * Clients send time stamp encoded in nonce. Make sure it is not too old,
 * to prevent replay attacks.
 * Assumption: nonce is a hexadecimal number of seconds since 1970.
 */
static int mg_check_nonce(struct mg_str nonce)
{
	unsigned long now = (unsigned long) mg_time();
	unsigned long val = (unsigned long) strtoul(nonce.ptr, NULL, 16);
	return (now >= val) && (now - val < 60 * 60);
}

static int mg_check_digest_auth(struct mg_str method, struct mg_str uri,
						 struct mg_str username, struct mg_str cnonce,
						 struct mg_str response, struct mg_str qop,
						 struct mg_str nc, struct mg_str nonce,
						 struct mg_str auth_domain, FILE *fp)
{
	char buf[128], f_user[sizeof(buf)], f_ha1[sizeof(buf)], f_domain[sizeof(buf)];
	char exp_resp[33];

	/*
	 * Read passwords file line by line. If should have htdigest format,
	 * i.e. each line should be a colon-separated sequence:
	 * USER_NAME:DOMAIN_NAME:MD5_HASH_OF_USER_DOMAIN_AND_PASSWORD
	 */
	while (fgets(buf, sizeof(buf), fp) != NULL) {
		if (sscanf(buf, "%[^:]:%[^:]:%s", f_user, f_domain, f_ha1) == 3 &&
			mg_vcmp(&username, f_user) == 0 &&
			mg_vcmp(&auth_domain, f_domain) == 0) {
			/* Username and domain matched, check the password */
			mg_mkmd5resp(method, uri, mg_str_s(f_ha1), nonce, nc, cnonce, qop, exp_resp);
			return mg_ncasecmp(response.ptr, exp_resp, strlen(exp_resp)) == 0;
		}
	}

	/* None of the entries in the passwords file matched - return failure */
	return 0;
}

static int mg_http_check_digest_auth(struct mg_http_message *hm, struct mg_str auth_domain, FILE *fp)
{
	struct mg_str *hdr;
	struct mg_str username, cnonce, response, qop, nc, nonce;

	/* Parse "Authorization:" header, fail fast on parse error */
	if (hm == NULL ||
		(hdr = mg_http_get_header(hm, "Authorization")) == NULL ||
		(username = mg_http_get_header_var(*hdr, mg_str_n("username", 8))).len == 0 ||
		(cnonce = mg_http_get_header_var(*hdr, mg_str_n("cnonce", 6))).len == 0 ||
		(response = mg_http_get_header_var(*hdr, mg_str_n("response", 8))).len == 0 ||
		mg_http_get_header_var(*hdr, mg_str_n("uri", 3)).len == 0 ||
		(qop = mg_http_get_header_var(*hdr, mg_str_n("qop", 3))).len == 0 ||
		(nc = mg_http_get_header_var(*hdr, mg_str_n("nc", 2))).len == 0 ||
		(nonce = mg_http_get_header_var(*hdr, mg_str_n("nonce", 5))).len == 0 ||
		mg_check_nonce(nonce) == 0) {
		return 0;
	}

	/* NOTE(lsm): due to a bug in MSIE, we do not compare URIs */

	return mg_check_digest_auth(
			hm->method,
			mg_str_n(
					hm->uri.ptr,
					hm->uri.len + (hm->query.len ? hm->query.len + 1 : 0)),
			username, cnonce, response, qop, nc, nonce, auth_domain, fp);
}

static int mg_http_is_authorized(struct mg_http_message *hm, const char *domain, const char *passwords_file) {
	FILE *fp;
	int authorized = 1;

	if (domain != NULL && passwords_file != NULL) {
		fp = fopen(passwords_file, "r");
		if (fp != NULL) {
			authorized = mg_http_check_digest_auth(hm, mg_str(domain), fp);
			fclose(fp);
		}
	}

	return authorized;
}

static void mg_http_send_digest_auth_request(struct mg_connection *c, const char *domain)
{
	mg_printf(c,
			  "HTTP/1.1 401 Unauthorized\r\n"
			  "WWW-Authenticate: Digest qop=\"auth\", "
			  "realm=\"%s\", nonce=\"%lx\"\r\n"
			  "Content-Length: 0\r\n\r\n",
			  domain, (unsigned long) mg_time());
	c->is_draining = 1;
}

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

static void restart_handler(struct mg_connection *nc, void *ev_data)
{
	struct mg_http_message *hm = (struct mg_http_message *) ev_data;
	ipc_message msg = {};

	if(mg_vcasecmp(&hm->method, "POST") != 0) {
		mg_http_reply(nc, 405, "", "%s", "Method Not Allowed\n");
		return;
	}

	int ret = ipc_postupdate(&msg);
	if (ret || msg.type != ACK) {
		mg_http_reply(nc, 500, "", "%s", "Failed to queue command\n");
		return;
	}

	mg_http_reply(nc, 201, "", "%s", "Device will reboot now.\n");
}

static void broadcast_callback(struct mg_connection *nc, int ev,
		void __attribute__ ((__unused__)) *ev_data, void __attribute__ ((__unused__)) *fn_data)
{
	static uint64_t last_io_time = 0;
	if (ev == MG_EV_READ) {
		struct mg_connection *t;
		for (t = nc->mgr->conns; t != NULL; t = t->next) {
			if (!t->is_websocket) continue;
			mg_ws_send(t,(char *)nc->recv.buf, nc->recv.len, WEBSOCKET_OP_TEXT);
		}
		mg_iobuf_del(&nc->recv, 0, nc->recv.len);
		last_io_time = mg_millis();
	} else if (ev == MG_EV_POLL) {
		struct mg_connection *t;
		uint64_t now = *((uint64_t *)ev_data);
		if (now < last_io_time + 20000) return;
		for (t = nc->mgr->conns; t != NULL; t = t->next) {
			if (!t->is_websocket) continue;
			mg_ws_send(t, "", 0, WEBSOCKET_OP_PING);
		}
		last_io_time = now;
	}
}

static int level_to_rfc_5424(int level)
{
	switch(level) {
		case ERRORLEVEL:
			return 3;
		case WARNLEVEL:
			return 4;
		case INFOLEVEL:
			return 6;
		case TRACELEVEL:
		case DEBUGLEVEL:
		default:
			return 7;
	}
}

static void broadcast(char *str)
{
	send(ws_pipe, str, strlen(str), 0);
}

static void *broadcast_message_thread(void __attribute__ ((__unused__)) *data)
{
	int fd = -1;

	for (;;) {
		ipc_message msg;
		int ret;

		if (fd < 0)
			fd = ipc_notify_connect();
		/*
		 * if still fails, try later
		 */
		if (fd < 0) {
			sleep(1);
			continue;
		}

		ret = ipc_notify_receive(&fd, &msg);
		if (ret != sizeof(msg))
			return NULL;

		if (strlen(msg.data.notify.msg) != 0 &&
				msg.data.status.current != PROGRESS) {
			char text[4096];
			char str[4160];

			snescape(text, sizeof(text), msg.data.notify.msg);

			snprintf(str, sizeof(str),
					 "{\r\n"
					 "\t\"type\": \"message\",\r\n"
					 "\t\"level\": \"%d\",\r\n"
					 "\t\"text\": \"%s\"\r\n"
					 "}\r\n",
					 level_to_rfc_5424(msg.data.notify.level), /* RFC 5424 */
					 text);

			broadcast(str);
		}
	}
}

static void *broadcast_progress_thread(void __attribute__ ((__unused__)) *data)
{
	RECOVERY_STATUS status = -1;
	sourcetype source = -1;
	unsigned int step = 0;
	uint8_t percent = 0;
	int fd = -1;

	for (;;) {
		struct progress_msg msg;
		char str[512];
		char escaped[512];
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

		if (msg.status != PROGRESS &&
		    (msg.status != status || msg.status == FAILURE)) {
			status = msg.status;

			snescape(escaped, sizeof(escaped), get_status_string(msg.status));

			snprintf(str, sizeof(str),
				"{\r\n"
				"\t\"type\": \"status\",\r\n"
				"\t\"status\": \"%s\"\r\n"
				"}\r\n",
				escaped);
			broadcast(str);
		}

		if (msg.source != source) {
			source = msg.source;

			snprintf(str, sizeof(str),
				"{\r\n"
				"\t\"type\": \"source\",\r\n"
				"\t\"source\": \"%s\"\r\n"
				"}\r\n",
				get_source_string(msg.source));
			broadcast(str);
		}

		if (msg.status == SUCCESS && msg.source == SOURCE_WEBSERVER && run_postupdate) {
			ipc_message ipc = {};

			ipc_postupdate(&ipc);
		}

		if (msg.infolen) {
			snescape(escaped, sizeof(escaped), msg.info);

			snprintf(str, sizeof(str),
				"{\r\n"
				"\t\"type\": \"info\",\r\n"
				"\t\"source\": \"%s\"\r\n"
				"}\r\n",
				escaped);
			broadcast(str);
		}

		if ((msg.cur_step != step || msg.cur_percent != percent) &&
				msg.cur_step) {
			step = msg.cur_step;
			percent = msg.cur_percent;

			snescape(escaped, sizeof(escaped), msg.cur_step ? msg.cur_image: "");

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
				escaped,
				msg.cur_percent);
			broadcast(str);
		}
	}
}

static void timer_ev_handler(void *fn_data)
{
	struct file_upload_state *fus = (struct file_upload_state *) fn_data;
	/*
	 * Check if a multi-part was initiated
	 */
	if (fus && (watchdog_conn > 0) &&
		(mg_millis() - fus->last_io_time > (watchdog_conn * 1000))) {
		/* Connection lost, drop data */
		ERROR("Connection lost, no data for %" PRId64 " seconds, closing...",
			  (mg_millis() - fus->last_io_time) / 1000);
		mg_http_reply(fus->c, 408, "", "%s", "Request Timeout\n");
		fus->c->is_draining = 1;
		mg_timer_free(&fus->c->mgr->timers, fus->timer);
	}
}

/*
 * Code common to V1 and V2
 */
static void upload_handler(struct mg_connection *nc, int ev, void *ev_data,
		void __attribute__ ((__unused__)) *fn_data)
{
	struct mg_http_multipart *mp;
	struct file_upload_state *fus;
	unsigned int percent;
	ssize_t written;

	switch (ev) {
		case MG_EV_HTTP_PART_BEGIN:
			mp = (struct mg_http_multipart *) ev_data;

			fus = (struct file_upload_state *) calloc(1, sizeof(struct file_upload_state));
			if (fus == NULL) {
				mg_http_reply(nc, 500, "", "%s", "Out of memory\n");
				nc->is_draining = 1;
				break;
			}
			fus->c = nc;

			struct swupdate_request req;
			swupdate_prepare_req(&req);
			req.len = mp->len;
			strncpy(req.info, mp->part.filename.ptr, sizeof(req.info) - 1);
			req.source = SOURCE_WEBSERVER;
			fus->fd = ipc_inst_start_ext(&req, sizeof(req));
			if (fus->fd < 0) {
				mg_http_reply(nc, 500, "", "%s", "Failed to queue command\n");
				nc->is_draining = 1;
				free(fus);
				break;
			}

			swupdate_download_update(0, mp->len);

			if (swupdate_file_setnonblock(fus->fd, true)) {
				WARN("IPC cannot be set in non-blocking, fallback to block mode");
			}

			mp->user_data = fus;

			fus->last_io_time = mg_millis();

			if (watchdog_conn > 0) {
				TRACE("Setting Webserver Watchdog Timer to %d", watchdog_conn);
				fus->timer = mg_timer_add(fus->c->mgr, 1000, MG_TIMER_REPEAT,
										  timer_ev_handler, fus);
			}

			break;

		case MG_EV_HTTP_PART_DATA:
			mp = (struct mg_http_multipart *) ev_data;
			fus = (struct file_upload_state *) mp->user_data;

			if (!fus)
				break;

			written = write(fus->fd, (char *) mp->part.body.ptr, mp->part.body.len);
			/*
			 * IPC seems to block, wait for a while
			 */
			if (written != mp->part.body.len) {
				if (written < 0) {
					if (errno != EAGAIN && errno != EWOULDBLOCK) {
						if ((mp->part.body.len + fus->len) == mp->len) {
							/*
							 * Simply consumes the data to unblock the sender
							 */
							written = (ssize_t) mp->part.body.len;
						} else if (!fus->error_report) {
							ERROR("Writing to IPC fails due to %s", strerror(errno));
							fus->error_report = true;
							nc->is_draining = 1;
						}
					} else
						written = 0;
				}
				usleep(100);
			}

			mp->num_data_consumed = written;
			fus->len += written;
			percent = (uint8_t)(100.0 * ((double)fus->len / (double)mp->len));
			if (percent != fus->percent) {
				fus->percent = percent;
				swupdate_download_update(fus->percent, mp->len);
			}

			fus->last_io_time = mg_millis();

			break;

		case MG_EV_HTTP_PART_END:
			mp = (struct mg_http_multipart *) ev_data;
			fus = (struct file_upload_state *) mp->user_data;

			if (!fus)
				break;

			ipc_end(fus->fd);

			mg_http_reply(nc, 200, "%s",
								  "Content-Type: text/plain\r\n"
								  "Connection: close");
			mg_send(nc, "\r\n", 2);
			mg_printf(nc, "Ok, %s - %d bytes.\r\n", mp->part.filename, (int) fus->len);
			nc->is_draining = 1;

			mp->user_data = NULL;
			mg_timer_free(&fus->c->mgr->timers, fus->timer);
			free(fus);
			break;
	}
}

static void websocket_handler(struct mg_connection *nc, void *ev_data)
{
	struct mg_http_message *hm = (struct mg_http_message *) ev_data;
	mg_ws_upgrade(nc, hm, NULL);
}

static void ev_handler(struct mg_connection *nc, int ev, void *ev_data, void *fn_data)
{
	if (nc->data[0] != 'M' && ev == MG_EV_HTTP_MSG) {
		struct mg_http_message *hm = (struct mg_http_message *) ev_data;
		if (!mg_http_is_authorized(hm, global_auth_domain, global_auth_file))
			mg_http_send_digest_auth_request(nc, global_auth_domain);
		else if (mg_http_get_header(hm, "Sec-WebSocket-Key") != NULL)
			websocket_handler(nc, ev_data);
		else if (mg_http_match_uri(hm, "/restart"))
			restart_handler(nc, ev_data);
		else
			mg_http_serve_dir(nc, ev_data, &s_http_server_opts);
	} else if (nc->data[0] != 'M' && ev == MG_EV_READ) {
		struct mg_http_message hm;
		int hlen = mg_http_parse((char *) nc->recv.buf, nc->recv.len, &hm);
		if (hlen > 0) {
			if (mg_http_match_uri(&hm, "/upload")) {
				if (!mg_http_is_authorized(&hm, global_auth_domain, global_auth_file)) {
					if (nc->pfn != NULL)
						mg_http_send_digest_auth_request(nc, global_auth_domain);
					nc->pfn = NULL;
					nc->pfn_data = NULL;
				} else {
					nc->pfn = upload_handler;
					nc->pfn_data = NULL;
					multipart_upload_handler(nc, MG_EV_HTTP_CHUNK, &hm, NULL);
				}
			}
		}
	} else if (nc->data[0] == 'M' && (ev == MG_EV_READ || ev == MG_EV_POLL || ev == MG_EV_CLOSE)) {
		if (nc->recv.len >= MG_MAX_RECV_SIZE && ev == MG_EV_READ)
			nc->is_full = true;
		multipart_upload_handler(nc, ev, ev_data, fn_data);
		if (nc->recv.len < MG_MAX_RECV_SIZE && ev == MG_EV_POLL)
			nc->is_full = false;
#if MG_ENABLE_SSL
	} else if (ev == MG_EV_ACCEPT && ssl) {
		mg_tls_init(nc, &tls_opts);
#endif
	} else if (ev == MG_EV_ERROR) {
		ERROR("%p %s", nc->fd, (char *) ev_data);
	} else if (ev == MG_EV_WS_MSG) {
		mg_iobuf_del(&nc->recv, 0, nc->recv.len);
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
	char *url = NULL;
	char buf[50] = "\0";
	int choice;

#if MG_ENABLE_SSL
	ssl = false;
#endif

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

	s_http_server_opts.root_dir =
		opts.root ? opts.root : MG_ROOT;
	if (!opts.listing)
		s_http_server_opts.fs = &fs_posix_no_list;
	global_auth_file = opts.global_auth_file;
	global_auth_domain = opts.auth_domain;

#if MG_ENABLE_SSL
	if (ssl) {
		tls_opts.cert = opts.ssl_cert;
		tls_opts.certkey = opts.ssl_key;
	}
#endif

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	mg_mgr_init(&mgr);

	ws_pipe = mg_mkpipe(&mgr, broadcast_callback, NULL, true);

	/* Parse url with port only fallback */
	if (opts.port) {
		if (mg_url_port(opts.port) != 0) {
			url = strdup(opts.port);
		} else {
			char *end;
			errno = 0;
			unsigned long port = strtoul(opts.port, &end, 10);
			if (opts.port == end || errno || *end || port > 65535)
				url = strdup(opts.port);
			else
				url = mg_mprintf(":%lu", port);
		}
		free(opts.port);
	} else {
		url = mg_mprintf(":%s", MG_PORT);
	}

	nc = mg_http_listen(&mgr, url, ev_handler, NULL);
	if (nc == NULL) {
		ERROR("Failed to start Mongoose.");
		exit(EXIT_FAILURE);
	}

	start_thread(broadcast_message_thread, NULL);
	start_thread(broadcast_progress_thread, NULL);

	mg_snprintf(buf, sizeof(buf), "%I", 4, &nc->loc.ip);
	INFO("Mongoose web server v%s with PID %d listening on %s:%" PRIu16 " and serving %s",
		MG_VERSION, getpid(), buf, mg_ntohs(nc->loc.port), s_http_server_opts.root_dir);

	while (s_signo == 0)
		mg_mgr_poll(&mgr, 100);
	mg_mgr_free(&mgr);

	free(url);

	return 0;
}
