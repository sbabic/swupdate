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
#ifdef CONFIG_SURICATTA_SSL
#include <openssl/sha.h>
#endif

/* hawkBit Channel Implementation Private Header File.
 *
 * This is a "private" header for testability, i.e., the declarations and
 * definitions herein should be used by `server_hawkbit.c`, `channel_hawkbit.c`,
 * and unit tests only.
 */

typedef struct {
	char *url;
	char *json_string;
	json_object *json_reply;
	bool debug;
	unsigned char retries;
	bool strictssl;
	char _pad;
	unsigned int retry_sleep;
#ifdef CONFIG_SURICATTA_SSL
	char sha1hash[SHA_DIGEST_LENGTH * 2 + 1];
	char _pad2[7];
#endif
} channel_data_t;
