/*
 * (C) Copyright 2008-2020
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <inttypes.h>
#include <unistd.h>
#include "network_ipc.h"
#include "progress_ipc.h"

static pthread_t async_thread_id;

struct async_lib {
	int connfd;
	int status;
	writedata	wr;
	getstatus	get;
	terminated	end;
};

static enum async_thread_state {
	ASYNC_THREAD_INIT,
	ASYNC_THREAD_RUNNING,
	ASYNC_THREAD_DONE
} running = ASYNC_THREAD_INIT;

static struct async_lib request;

#define get_request()	(&request)

/* Wait until the end of the installation (FAILURE or SUCCESS)
 *
 * Arguments:
 *   progressfd: File descriptor for IPC with the daemon.
 *               Will be closed and set to -1 before return.
 *
 * Return value:
 *   FAILURE or SUCCESS
 */
static RECOVERY_STATUS inst_wait_for_complete(int *progressfd)
{
	struct progress_msg progressmsg;
	RECOVERY_STATUS result = FAILURE;
	int ret = -1;

	/* Wait until the end of the installation (FAILURE or SUCCESS) */
	while (1) {
		ret = progress_ipc_receive(progressfd, &progressmsg);
		if (ret < 0) {
			/* Note that progressfd may have been closed by progress_ipc_receive
			 * and set to -1 */
			fprintf(stderr, "progress_ipc_receive failed (%d)\n", ret);
			result = FAILURE;
			break;
		}

		if (progressmsg.status == FAILURE || progressmsg.status == SUCCESS) {
			/* We have the final result of the installation */
			result = progressmsg.status;
			break;
		} else {
			/* Other status (START, RUN, PROGRESS) */
		}
	}

	if (*progressfd >= 0) {
		close(*progressfd);
		*progressfd = -1;
	}

	return result;
}

/* Get all status messages from the server and print them
 * until ipc_get_status() returns IDLE.
 */
static void unstack_installation_status(getstatus callback)
{
	ipc_message ipcmsg;
	int previous_status = -1;
	int ret = -1;

	do {
		ret = ipc_get_status(&ipcmsg);
		if (ret < 0)
			break;

		/* print if the status changed or there is a description */
		if ( (previous_status != ipcmsg.data.status.current) ||
		     strlen(ipcmsg.data.status.desc) ) {
			callback(&ipcmsg);
		}
		previous_status = ipcmsg.data.status.current;

	} while (ipcmsg.data.status.current != IDLE);
}

/* Consume progress events
 *
 * Returns:
 *        -1 error
 *   FAILURE the installation failed
 *   SUCCESS the installation suceeded
 *         0 no event or other events consumed
 *
 * On error, progressfd is closed and set to -1.
 */
static int consume_progress_events(int *progressfd)
{
	struct progress_msg progressmsg;
	int ret = -1;

	/* Wait until the end of the installation (FAILURE or SUCCESS) */
	while (1) {
		int err = progress_ipc_receive_nb(progressfd, &progressmsg);
		if (err < 0) {
			/* Note that progressfd may have been closed by progress_ipc_receive
			 * and set to -1 */
			fprintf(stderr, "progress_ipc_receive_nb failed (%d)\n", ret);
			ret = -1;
			break;
		} else if (err == 0) {
			/* no pending message */
			ret = 0;
			break;
		}

		if (progressmsg.status == FAILURE || progressmsg.status == SUCCESS) {
			/* We have the final result of the installation */
			ret = progressmsg.status;
			break;
		} else {
			/* Other status (START, RUN, PROGRESS) */
			/* continue consuming messages */
			continue;
		}
	}

	if (ret == -1 && *progressfd >= 0) {
		close(*progressfd);
		*progressfd = -1;
	}

	return ret;
}

static void *swupdate_async_thread(void *data)
{
	char *pbuf;
	int size;
	sigset_t sigpipe_mask;
	sigset_t saved_mask;
	struct timespec zerotime = {0, 0};
	struct async_lib *rq = (struct async_lib *)data;
	int swupdate_result = FAILURE;
	int progressfd = -1;
	int ret;
	int early_status = -1;

	sigemptyset(&sigpipe_mask);
	sigaddset(&sigpipe_mask, SIGPIPE);

	if (pthread_sigmask(SIG_BLOCK, &sigpipe_mask, &saved_mask) == -1) {
		perror("pthread_sigmask");
		swupdate_result = FAILURE;
		goto out;
	}
	/* Start listening to progress events, before sending
	 * the image so that we don't miss the result event.
	 */
	progressfd = progress_ipc_connect(0 /* no reconnect */);
	if (progressfd < 0) {
		fprintf(stderr, "progress_ipc_connect failed\n");
		ipc_end(rq->connfd);
		goto out;
	}

	/* Start writing the image until end */

	do {
		if (!rq->wr)
			break;

		rq->wr(&pbuf, &size);
		if (size) {
			if (swupdate_image_write(pbuf, size) != size) {
				perror("swupdate_image_write failed");
				swupdate_result = FAILURE;
				goto out;
			}
		}
		/* Consume progress events so that the pipe does not get full
		 * and block the daemon */
		ret = consume_progress_events(&progressfd);
		if (ret == -1) {
			/* If we cannot get events, then we won't be able to get the result.
			 * Quit and fail */
			fprintf(stderr, "Cannot consume progress events. Fail.\n");
			early_status = FAILURE;
			break;
		} else if (ret == FAILURE || ret == SUCCESS) {
			/* early termination */
			fprintf(stderr, "early termination while sending the image: %s\n",
			        ret==SUCCESS?"SUCCESS":"FAILURE");
			early_status = ret;
			/* interrupt the transfer */
			break;
		}
	} while(size > 0);

	ipc_end(rq->connfd);

	/*
	 * Everything sent, wait for completion of the installation
	 */

	if (early_status >= 0) {
		swupdate_result = early_status;
		close(progressfd);
		progressfd = -1;

	} else {
		/* Wait until the end of the installation and get the final result */
		swupdate_result = inst_wait_for_complete(&progressfd); /* progressfd closed by the call */
	}

	/*
	 * Get and print all status lines, for compatibility with legacy programs
	 * that expect them
	 */
	if (rq->get) unstack_installation_status(rq->get);
 
	if (sigtimedwait(&sigpipe_mask, 0, &zerotime) == -1) {
		// currently ignored
	}

	if (pthread_sigmask(SIG_SETMASK, &saved_mask, 0) == -1) {
		perror("pthread_sigmask");
		swupdate_result = FAILURE;
		goto out;
	}

out:
	running = ASYNC_THREAD_DONE;
	if (rq->end)
		rq->end((RECOVERY_STATUS)swupdate_result);

	pthread_exit((void*)(intptr_t)(swupdate_result == SUCCESS));
}

/*
 * This is duplicated from pctl
 * to let build the ipc library without
 * linking pctl code
 */
static void start_ipc_thread(void *(* start_routine) (void *), void *arg)
{
	int ret;
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	ret = pthread_create(&async_thread_id, &attr, start_routine, arg);
	if (ret) {
		perror("ipc thread creation failed");
		return;
	}

	running = ASYNC_THREAD_RUNNING;
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

	switch (running) {
	case ASYNC_THREAD_INIT:
		break;
	case ASYNC_THREAD_DONE:
		pthread_join(async_thread_id, NULL);
		running = ASYNC_THREAD_INIT;
		break;
	default:
		return -EBUSY;
	}

	rq = get_request();

	rq->wr = wr_func;
	rq->get = status_func;
	rq->end = end_func;

	connfd = ipc_inst_start_ext(priv, size);

	if (connfd < 0)
		return connfd;

	rq->connfd = connfd;

	start_ipc_thread(swupdate_async_thread, rq);

	return running != ASYNC_THREAD_INIT;
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
int swupdate_set_version_range_type(const char *updatetype,
				const char *minversion,
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

	if (updatetype) {
		strncpy(msg.data.versions.update_type,
			updatetype,
			sizeof(msg.data.versions.update_type) - 1);
	}
	return ipc_send_cmd(&msg);
}

int swupdate_set_version_range(const char *minversion,
				const char *maxversion,
				const char *currentversion)

{
	return swupdate_set_version_range_type(minversion,
						      maxversion,
						      currentversion,
						      NULL);
}

int swupdate_dwl_url (const char *artifact_name, const char *url)
{
	ipc_message msg;

	memset(&msg, 0, sizeof(msg));
	msg.magic = IPC_MAGIC;
	msg.type = SET_DELTA_URL;

	strlcpy(msg.data.dwl_url.filename,
			artifact_name,
			sizeof(msg.data.dwl_url.filename) - 1);
	strlcpy(msg.data.dwl_url.url,
			url,
			sizeof(msg.data.dwl_url.url) - 1);
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
