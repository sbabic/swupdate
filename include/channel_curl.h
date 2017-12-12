/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#pragma once
#include <json-c/json.h>
#include <stdio.h>
#include "sslapi.h"

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

#define FD_USE_IPC -2

#define USE_PROXY_ENV (char *)0x11

typedef struct {
	char *url;
	char *json_string;
	json_object *json_reply;
	char *cafile;
	char *sslkey;
	char *sslcert;
	char *proxy;
	char *info;
	unsigned int retry_sleep;
	unsigned int offs;
	unsigned int method;
	unsigned int retries;
	bool debug;
	bool usessl;
	bool strictssl;
	int (*checkdwl)(void);
	struct swupdate_digest *dgst;
	char sha1hash[SHA_DIGEST_LENGTH * 2 + 1];
} channel_data_t;
