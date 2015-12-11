/*
 * (C) Copyright 2008-2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#include <pthread.h>

#include "util.h"
#include "network_ipc.h"

struct async_lib {
	int connfd;
	int status;
	writedata	wr;
	getstatus	get;
	terminated	end;
};

static int handle = 0;
struct async_lib request;
static pthread_t async_thread_id;

#define get_request()	(&request)

static int prepare_ipc(void) {

	int connfd;
	int ret;

	struct sockaddr_un servaddr;

	connfd = socket(AF_LOCAL, SOCK_STREAM, 0);
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sun_family = AF_LOCAL;

	strcpy(servaddr.sun_path, SOCKET_CTRL_PATH);

	ret = connect(connfd, (struct sockaddr *) &servaddr, sizeof(servaddr));
	if (ret < 0) {
		close(connfd);
		return ret;
	}

	return connfd;
}

static int __ipc_get_status(int connfd, ipc_message *msg)
{
	int ret;

	memset(msg, 0, sizeof(*msg));
	msg->magic = IPC_MAGIC;
	msg->type = GET_STATUS;
	ret = write(connfd, msg, sizeof(*msg));
	if (ret != sizeof(*msg))
		return -1;

	ret = read(connfd, msg, sizeof(*msg));
	if (ret <= 0)
		return -1;

	return 0;
}

int ipc_get_status(ipc_message *msg)
{
	int ret;
	int connfd;

	connfd = prepare_ipc();
	if (connfd < 0) {
		return -1;
	}
	ret = __ipc_get_status(connfd, msg);
	close(connfd);

	return ret;
}

int ipc_inst_start(void)
{
	int connfd;
	ipc_message msg;
	int ret;

	connfd = prepare_ipc();
	if (connfd < 0)
		return -1;

	memset(&msg, 0, sizeof(msg));
	msg.magic = IPC_MAGIC;
	msg.type = REQ_INSTALL;

	ret = write(connfd, &msg, sizeof(msg));
	if (ret != sizeof(msg)) {
		close(connfd);
		return -1;
	}

	ret = read(connfd, &msg, sizeof(msg));
	if (ret <= 0) {
		close(connfd);
		return -1;
	}

	if (msg.type != ACK) {
		close(connfd);
		return -1;
	}

	return connfd;
}

/*
 * This is not required, it is really a wrapper for
 * write, but make interface consistent
 */
int ipc_send_data(int connfd, char *buf, int size)
{
	int ret;

	ret = write(connfd,  buf, size);
	if (ret != size) {
		return -1;
	}

	return ret;
}

void ipc_end(int connfd)
{
	close(connfd);
}

int swupdate_image_write(char *buf, int size)
{
	struct async_lib *rq;

	rq = get_request();

	return ipc_send_data(rq->connfd, buf, size);
}

int ipc_wait_for_complete(getstatus callback)
{
	int fd;
	RECOVERY_STATUS status = IDLE;
	ipc_message message;
	int ret;

	do {
		fd = prepare_ipc();
		if (fd < 0)
			break;
		ret = __ipc_get_status(fd, &message);
		ipc_end(fd);

		if (ret < 0) {
			printf("__ipc_get_status returned %d\n", ret);
			message.data.status.last_result = FAILURE;
			break;
		}

		if (( (status != message.data.status.current) ||
			strlen(message.data.status.desc))) {
				if (callback)
					callback(&message);
			} else
				sleep(1);

		status = message.data.status.current;
	} while(message.data.status.current != IDLE);

	return message.data.status.last_result;
}

static void *swupdate_async_thread(void *data)
{
	char *pbuf;
	int size;
	sigset_t sigpipe_mask;
	sigset_t saved_mask;
	struct timespec zerotime = {0};
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
	printf("Now getting status\n");

	/*
	 * Everything sent, ask for status
	 */

	swupdate_result = ipc_wait_for_complete(rq->get);

	handle = 0;

	if (sigtimedwait(&sigpipe_mask, 0, &zerotime) == -1) {
		// curently ignored
	}

	if (pthread_sigmask(SIG_SETMASK, &saved_mask, 0) == -1) {
		  perror("pthread_sigmask");
	}

	if (rq->end)
		rq->end(swupdate_result);

	pthread_exit(NULL);
}

/*
 * This is part of the library for an external client.
 * Only one running request is accepted
 */
int swupdate_async_start(writedata wr_func, getstatus status_func,
				terminated end_func)
{
	struct async_lib *rq;
	int connfd;

	if (handle)
		return -EBUSY;

	rq = get_request();

	rq->wr = wr_func;
	rq->get = status_func;
	rq->end = end_func;

	connfd = ipc_inst_start();

	if (connfd < 0)
		return connfd;

	rq->connfd = connfd;

	async_thread_id = start_thread(swupdate_async_thread, rq);

	handle++;

	return handle;
}

pthread_t start_thread(void *(* start_routine) (void *), void *arg)
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
