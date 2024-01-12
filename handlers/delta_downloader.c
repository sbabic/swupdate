/*
 * (C) Copyright 2021
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * This is part of the delta handler. It is started as separate process
 * and gets from the main task which chunks should be downloaded.
 * The main task just sends a RANGE Request, and the downloader start
 * a curl connection to the server and sends the received data back to the main task.
 * The IPC is message oriented, and process add small metadata
 * information to inform if the download reports errors (from libcurl).
 * This is used explicitely to retrieve ranges : an answer
 * different as "Partial Content" (206) is rejected. This avoids that the
 * whole file is downloaded if the server is not able to work with ranges.
 */

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <util.h>
#include <pctl.h>
#include <zlib.h>
#include <channel.h>
#include <channel_curl.h>
#include "swupdate_dict.h"
#include "delta_handler.h"
#include "delta_process.h"
#include "swupdate_settings.h"
#include "server_utils.h"

/*
 * Structure used in curl callbacks
 */
typedef struct {
	unsigned int id;	/* Request id */
	int writefd;		/* IPC file descriptor */
	range_answer_t *answer;
} dwl_data_t;

extern channel_op_res_t channel_curl_init(void);

static channel_data_t channel_data_defaults = {
					.debug = false,
					.source=SOURCE_CHUNKS_DOWNLOADER,
					.retries=CHANNEL_DEFAULT_RESUME_TRIES,
					.retry_sleep=
						CHANNEL_DEFAULT_RESUME_DELAY,
					.nocheckanswer=false,
					.nofollow=false,
					.connection_timeout=0,
					.headers_to_send = NULL,
					.received_headers = NULL
					};

/*
 * Data callback: takes the buffer, surrounded with IPC meta data
 * and send to the process that reqeusted the download
 */
static size_t wrdata_callback(char *buffer, size_t size, size_t nmemb, void *data)
{
	if (!data)
		return 0;

	channel_data_t *channel_data = (channel_data_t *)data;
	dwl_data_t *dwl = (dwl_data_t *)channel_data->user;
	ssize_t nbytes = nmemb * size;
	int ret;
	if (!nmemb) {
		return 0;
	}

	if (channel_data->http_response_code != 206) {
		ERROR("Bytes request not supported by server, returning %ld",
			channel_data->http_response_code);
		return 0;
	}
	while (nbytes > 0) {
		range_answer_t *answer = dwl->answer;
		answer->id = dwl->id;
		answer->type = RANGE_DATA;
		answer->len = min(nbytes, RANGE_PAYLOAD_SIZE);
		memcpy(answer->data, buffer, answer->len);
		answer->crc = crc32(0, (unsigned char *)answer->data, answer->len);
		ret = copy_write(&dwl->writefd, answer, sizeof(range_answer_t));
		if (ret < 0) {
			ERROR("Error sending IPC data !");
			return 0;
		}
		nbytes -= answer->len;
	}

	return size * nmemb;
}

/*
 * This function just extract the header and sends
 * to the process initiating the transfer.
 * It envelops the header in the answer struct
 * The receiver knows from meta data if payload contains headers
 * or data.
 * A single header is encapsulated in one IPC message.
 */
static size_t delta_callback_headers(char *buffer, size_t size, size_t nitems, void *data)
{
	channel_data_t *channel_data = (channel_data_t *)data;
	dwl_data_t *dwl = (dwl_data_t *)channel_data->user;
	int ret;

	range_answer_t *answer = dwl->answer;
	answer->id = dwl->id;
	answer->type = RANGE_HEADERS;
	answer->len = min(size * nitems , RANGE_PAYLOAD_SIZE - 2);
	memcpy(answer->data, buffer, answer->len);
	answer->len++;
	answer->data[answer->len] = '\0';

	ret = write(dwl->writefd, answer, sizeof(range_answer_t));
	if (ret != sizeof(range_answer_t)) {
		ERROR("Error sending IPC data !");
		return 0;
	}

	return nitems * size;
}

/*
 * Process that is spawned by the handler to download the missing chunks.
 * Downloading should be done in a separate process to not break
 * privilige separation
 */
int start_delta_downloader(const char __attribute__ ((__unused__)) *fname,
				int __attribute__ ((__unused__)) argc,
				__attribute__ ((__unused__)) char *argv[])
{
	ssize_t ret;
	range_request_t *req = NULL;
	channel_op_res_t transfer;
	range_answer_t *answer;
	struct dict httpheaders;
	dwl_data_t priv;

	TRACE("Starting Internal process for downloading chunks");
	if (channel_curl_init() != CHANNEL_OK) {
		ERROR("Cannot initialize curl");
		return SERVER_EINIT;
	}
	req = (range_request_t *)malloc(sizeof *req);
	if (!req) {
		ERROR("OOM requesting request buffers !");
		exit (EXIT_FAILURE);
	}

	answer = (range_answer_t *)malloc(sizeof *answer);
	if (!answer) {
		ERROR("OOM requesting answer buffers !");
		exit (EXIT_FAILURE);
	}

	channel_data_t channel_data = channel_data_defaults;
	channel_t *channel = channel_new();
	if (!channel) {
		ERROR("Cannot get channel for communication");
		exit (EXIT_FAILURE);
	}
	LIST_INIT(&httpheaders);
	if (dict_insert_value(&httpheaders, "Accept", "*/*")) {
		ERROR("Database error setting Accept header");
		exit (EXIT_FAILURE);
	}

	for (;;) {
		ret = read(sw_sockfd, req, sizeof(range_request_t));
		if (ret < 0) {
			ERROR("reading from sockfd returns error, aborting...");
			exit (EXIT_FAILURE);
		}

		if ((req->urllen + req->rangelen) > ret) {
			ERROR("Malformed data");
			continue;
		}
		priv.writefd = sw_sockfd;
		priv.id = req->id;
		priv.answer = answer;
		channel_data.url = req->data;
		channel_data.noipc = true;
		channel_data.method = CHANNEL_GET;
		channel_data.content_type = "*";
		channel_data.headers = delta_callback_headers;
		channel_data.dwlwrdata = wrdata_callback;
		channel_data.range = &req->data[req->urllen + 1];
		channel_data.user = &priv;

		swupdate_cfg_handle handle;
		swupdate_cfg_init(&handle);

		if (swupdate_cfg_read_file(&handle, fname) == 0) {
			read_module_settings(&handle, "delta", channel_settings, &channel_data);
		}

		swupdate_cfg_destroy(&handle);

		if (channel->open(channel, &channel_data) == CHANNEL_OK) {
			transfer = channel->get_file(channel, (void *)&channel_data);
		} else {
			ERROR("Cannot open channel for communication");
			transfer = CHANNEL_EINIT;
		}

		answer->id = req->id;
		answer->type = (transfer == CHANNEL_OK) ? RANGE_COMPLETED : RANGE_ERROR;
		answer->len = 0;
		if (write(sw_sockfd, answer, sizeof(*answer)) != sizeof(*answer)) {
			ERROR("Answer cannot be sent back, maybe deadlock !!");
		}

		(void)channel->close(channel);
	}

	exit (EXIT_SUCCESS);
}
