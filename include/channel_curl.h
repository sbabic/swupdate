/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#pragma once
#ifdef CONFIG_JSON
#include <json-c/json.h>
#endif
#include <stdio.h>
#include <stdbool.h>
#include "sslapi.h"
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
} channel_method_t;

#define USE_PROXY_ENV (char *)0x11

typedef struct {
	char *url;
	char *auth;
	char *request_body;
#ifdef CONFIG_JSON
	json_object *json_reply;
#endif
	char *cafile;
	char *sslkey;
	char *sslcert;
	char *proxy;
	char *info;
	char *header;
	unsigned int retry_sleep;
	unsigned int offs;
	unsigned int method;
	unsigned int retries;
	unsigned int low_speed_timeout;
	bool debug;
	bool usessl;
	bool strictssl;
	int (*checkdwl)(void);
	struct swupdate_digest *dgst;
	char sha1hash[SHA_DIGEST_LENGTH * 2 + 1];
	sourcetype source;
} channel_data_t;
