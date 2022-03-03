/*
 * (C) Copyright 2008-2020
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include "network_ipc.h"

static pthread_t async_thread_id;

struct async_lib {
	int connfd;
	int status;
	writedata	wr;
	getstatus	get;
	terminated	end;
};

static int handle = 0;

static struct async_lib request;

#define get_request()	(&request)

static void *swupdate_async_thread(void *data)
{
	char *pbuf;
	int size;
	sigset_t sigpipe_mask;
	sigset_t saved_mask;
	struct timespec zerotime = {0, 0};
	struct async_lib *rq = (struct async_lib *)data;
	int swupdate_result;

	sigemptyset(&sigpipe_mask);
	sigaddset(&sigpipe_mask, SIGPIPE);

	if (pthread_sigmask(SIG_BLOCK, &sigpipe_mask, &saved_mask) == -1) {
		  perror("pthread_sigmask");
		    exit(1);
	}
	/* Start writing the image until end */

	do {
		if (!rq->wr)
			break;

		rq->wr(&pbuf, &size);
		if (size)
			swupdate_image_write(pbuf, size);
	} while(size > 0);

	ipc_end(rq->connfd);

	/*
	 * Everything sent, ask for status
	 */

	swupdate_result = ipc_wait_for_complete(rq->get);

	handle = 0;

	if (sigtimedwait(&sigpipe_mask, 0, &zerotime) == -1) {
		// currently ignored
	}

	if (pthread_sigmask(SIG_SETMASK, &saved_mask, 0) == -1) {
		  perror("pthread_sigmask");
	}

	if (rq->end)
		rq->end((RECOVERY_STATUS)swupdate_result);

	pthread_exit(NULL);
}

/*
 * This is duplicated from pctl
 * to let build the ipc library without
 * linking pctl code
 */
static pthread_t start_ipc_thread(void *(* start_routine) (void *), void *arg)
{
	int ret;
	pthread_t id;
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	ret = pthread_create(&id, &attr, start_routine, arg);
	if (ret) {
		exit(1);
	}
	return id;
}

/*
 * This is part of the library for an external client.
 * Only one running request is accepted
 */
int swupdate_async_start(writedata wr_func, getstatus status_func,
				terminated end_func, void *priv, ssize_t size)
{
	struct async_lib *rq;
	int connfd;

	if (handle)
		return -EBUSY;

	rq = get_request();

	rq->wr = wr_func;
	rq->get = status_func;
	rq->end = end_func;

	connfd = ipc_inst_start_ext(priv, size);

	if (connfd < 0)
		return connfd;

	rq->connfd = connfd;

	async_thread_id = start_ipc_thread(swupdate_async_thread, rq);

	handle++;

	return handle;
}

int swupdate_image_write(char *buf, int size)
{
	struct async_lib *rq;

	rq = get_request();

	return ipc_send_data(rq->connfd, buf, size);
}

/*
 * Set via IPC the AES key for decryption
 * key is passed as ASCII string
 */
int swupdate_set_aes(char *key, char *ivt)
{
	ipc_message msg;

	if (!key || !ivt)
		return -EINVAL;
	if (strlen(key) != 64 && strlen(ivt) != 32)
		return -EINVAL;

	memset(&msg, 0, sizeof(msg));

	msg.magic = IPC_MAGIC;
	msg.type = SET_AES_KEY;

	/*
	 * Lenght for key and IVT are fixed
	 */
	strncpy(msg.data.aeskeymsg.key_ascii, key, sizeof(msg.data.aeskeymsg.key_ascii) - 1);
	strncpy(msg.data.aeskeymsg.ivt_ascii, ivt, sizeof(msg.data.aeskeymsg.ivt_ascii) - 1);

	return ipc_send_cmd(&msg);
}

/*
 * Set via IPC the range of accepted versions
 * Versions are string and they can use semver
 */
int swupdate_set_version_range(const char *minversion,
				const char *maxversion,
				const char *currentversion)
{
	ipc_message msg;

	memset(&msg, 0, sizeof(msg));
	msg.magic = IPC_MAGIC;
	msg.type = SET_VERSIONS_RANGE;

	if (minversion) {
		strncpy(msg.data.versions.minimum_version,
			minversion,
			sizeof(msg.data.versions.minimum_version) - 1);
	}

	if (maxversion) {
		strncpy(msg.data.versions.maximum_version,
			maxversion,
			sizeof(msg.data.versions.maximum_version) - 1);
	}

	if (currentversion) {
		strncpy(msg.data.versions.current_version,
			currentversion,
			sizeof(msg.data.versions.maximum_version) - 1);
	}

	return ipc_send_cmd(&msg);
}

void swupdate_prepare_req(struct swupdate_request *req) {
	if (!req)
		return;
	memset(req, 0, sizeof(struct swupdate_request));
	req->apiversion = SWUPDATE_API_VERSION;
	req->dry_run = RUN_DEFAULT;
	return;
}
