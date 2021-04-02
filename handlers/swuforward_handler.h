/*
 * (C) Copyright 2017-2019
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _SWUFORWARD_HANDLER_H
#define _SWUFORWARD_HANDLER_H

#include <curl/curl.h>
#include "bsdqueue.h"
#include "channel_curl.h"
#include "channel.h"

/*
 * The Webserver in SWUpdate expets a custom header
 * with the filename
 */
#define CUSTOM_HEADER "X_FILENAME: "
#define MAX_WAIT_MS	3000
#define POST_URL_V2	"/upload"

/*
 * The handler checks if a remote update was successful
 * asking for the status. It is supposed that the boards go on
 * until they report a success or failure.
 * Following timeout is introduced in case boards answer, but they do not
 * go out for some reasons from the running state.
 */
#define TIMEOUT_GET_ANSWER_SEC		900	/* 15 minutes */
#define POLLING_TIME_REQ_STATUS		50	/* in mSec */

typedef enum {
	WS_UNKNOWN,
	WS_ESTABLISHED,
	WS_ERROR,
	WS_CLOSED
} SWUPDATE_WS_CONNECTION;

/*
 * Track each connection
 * The handler maintains a list of connections and sends the SWU
 * to all of them at once.
 */
struct curlconn {
	CURL *curl_handle;	/* CURL handle for posting image */
	curl_mime *form;	/* Used to set up mulitipart/form-data */
	struct curl_slist *headerlist; /* List of headers used for each conn */
	const void *buffer;	/* temporary buffer to transfer image */
	unsigned int nbytes;	/* bytes to be transferred */
	size_t total_bytes;	/* size of SWU image */
	int fifo[2];		/* Pipe for IPC */
	char *url;		/* URL for forwarding */
	bool gotMsg;		/* set if the remote board has sent a new msg */
	RECOVERY_STATUS SWUpdateStatus;	/* final status of update */
	channel_op_res_t response;
	void *ws;		/* this is used by websockets module */
	SWUPDATE_WS_CONNECTION connstatus;
	pthread_t transfer_thread;
	int exitval;
	LIST_ENTRY(curlconn) next;
};
LIST_HEAD(listconns, curlconn);

int swuforward_ws_connect(struct curlconn *conn);
int swuforward_ws_getanswer(struct curlconn *conn, int timeout);
void swuforward_ws_free(struct curlconn *conn);
#endif
