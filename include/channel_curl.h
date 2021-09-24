/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once
#include "util.h"
#ifdef CONFIG_JSON
#include <json-c/json.h>
#endif
#include <stdio.h>
#include <stdbool.h>
#include "swupdate_status.h"

/* Curl Channel Implementation Private Header File.
 *
 * This is a "private" header for testability, i.e., the declarations and
 * definitions herein should be used by code employing the curl channel
 * (e.g. server_hawkbit.c) and unit tests only.
 */

typedef enum {
	CHANNEL_GET,
	CHANNEL_POST,
	CHANNEL_PUT,
	CHANNEL_PATCH,
} channel_method_t;

typedef enum {
	CHANNEL_PARSE_NONE,
	CHANNEL_PARSE_JSON,
	CHANNEL_PARSE_RAW
} channel_body_t;

#define USE_PROXY_ENV (char *)0x11

typedef struct {
	char *url;
	char *cached_file;
	char *auth;
	char *request_body;
	char *iface;
#ifdef CONFIG_JSON
	json_object *json_reply;
#endif
	char *raw_reply;
	bool dry_run;
	char *cafile;
	char *sslkey;
	char *sslcert;
	char *ciphers;
	char *proxy;
	char *info;
	char *auth_token;
	const char *content_type;
	unsigned int retry_sleep;
	unsigned int offs;
	unsigned int method;
	unsigned int retries;
	unsigned int low_speed_timeout;
	unsigned int connection_timeout;
	channel_body_t format;
	bool debug;
	bool usessl;
	bool strictssl;
	bool nocheckanswer;
	bool noipc;	/* do not send to SWUpdate IPC if set */
	long http_response_code;
	bool nofollow;
	size_t (*dwlwrdata)(char *streamdata, size_t size, size_t nmemb,
				   void *data);
	size_t (*headers)(char *streamdata, size_t size, size_t nmemb,
				   void *data);
	struct swupdate_digest *dgst;
	char sha1hash[SWUPDATE_SHA_DIGEST_LENGTH * 2 + 1];
	sourcetype source;
	struct dict *headers_to_send;
	struct dict *received_headers;
	unsigned int max_download_speed;
	char *range; /* Range request for get_file in any */
	void *user;
} channel_data_t;
