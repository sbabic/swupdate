/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once
#include "util.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdbool.h>
#include "swupdate_status.h"

/** Curl Channel Implementation Header File.
 *
 * This is the specific channel implementation using libcurl.
 */

typedef enum {
	CHANNEL_GET,
	CHANNEL_POST,
	CHANNEL_PUT,
	CHANNEL_PATCH,
	CHANNEL_DELETE,
} channel_method_t;

/*
 * format for a response: the channel can just transfer
 * or try to parse the answer with a specific format
 */
typedef enum {
	CHANNEL_PARSE_NONE,
	CHANNEL_PARSE_JSON,
	CHANNEL_PARSE_RAW
} channel_body_t;

#define USE_PROXY_ENV (char *)0x11

/*
 * Structure to configure the connection and to
 * exchange data.
 * This is passed to the channel methods (defined in channel.h)
 * to set up the connection.
 */
typedef struct {
	char *url;		/* URL for connection */
	char *unix_socket;	/* if set, the UNIX Socket is taken for local connection */
	char *cached_file;	/* Retrieve file from cached file before getting from network */ 
	char *auth;
	char *request_body;	/* Buffer for the answer */
	char *iface;		/* Set a specific interface */
	json_object *json_reply;
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
	const char *accept_content_type;
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
	/*
	 * read_fifo is used as alternative to request_body buffer to push data
	 * in case of large data. This lets push a stream instead of a buffer.
	 */
	int read_fifo;
	size_t (*headers)(char *streamdata, size_t size, size_t nmemb,
				   void *data);
	struct swupdate_digest *dgst;
	char sha1hash[SWUPDATE_SHA_DIGEST_LENGTH * 2 + 1];
	sourcetype source;
	struct dict *headers_to_send;
	struct dict *received_headers;
	unsigned int max_download_speed;
	size_t	upload_filesize;
	char *range; /* Range request for get_file in any */
	void *user;
} channel_data_t;
