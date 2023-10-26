/*
 * Author: Christian Storm
 * Copyright (C) 2017, Siemens AG
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>

#include <progress_ipc.h>

#ifdef CONFIG_SOCKET_PROGRESS_PATH
char *SOCKET_PROGRESS_PATH = (char*)CONFIG_SOCKET_PROGRESS_PATH;
#else
char *SOCKET_PROGRESS_PATH = NULL;
#endif

#define SOCKET_PROGRESS_DEFAULT  "swupdateprog"

char *get_prog_socket(void) {
	if (!SOCKET_PROGRESS_PATH || !strlen(SOCKET_PROGRESS_PATH)) {
		const char *socketdir = getenv("RUNTIME_DIRECTORY");
		if(!socketdir){
			socketdir = getenv("TMPDIR");
		}
		if(!socketdir){
			socketdir = "/tmp";
		}
		if (asprintf(&SOCKET_PROGRESS_PATH, "%s/%s", socketdir, SOCKET_PROGRESS_DEFAULT) == -1)
			return (char *)"/tmp/"SOCKET_PROGRESS_DEFAULT;
	}

	return SOCKET_PROGRESS_PATH;
}

static int _progress_ipc_connect(const char *socketpath, bool reconnect)
{
	struct sockaddr_un servaddr;
	int fd = socket(AF_LOCAL, SOCK_STREAM, 0);
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sun_family = AF_LOCAL;
	strncpy(servaddr.sun_path, socketpath, sizeof(servaddr.sun_path) - 1);

	/*
	 * Check to get a valid socket
	 */
	if (fd < 0)
		return -1;

	do {
		if (connect(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) == 0) {
			break;
		}
		if (!reconnect) {
			fprintf(stderr, "cannot communicate with SWUpdate via %s\n", socketpath);
			close(fd);
			return -1;
		}

		usleep(10000);
	} while (true);

	return fd;
}

int progress_ipc_connect_with_path(const char *socketpath, bool reconnect) {
	return _progress_ipc_connect(socketpath, reconnect);
}

int progress_ipc_connect(bool reconnect)
{
	return _progress_ipc_connect(get_prog_socket(), reconnect);
}

int progress_ipc_receive(int *connfd, struct progress_msg *msg) {
	int ret = read(*connfd, msg, sizeof(*msg));

	if (ret == -1 && (errno == EAGAIN || errno == EINTR))
		return 0;

	/*
	 * size of message can vary if the API version does not match
	 * First check it to return a correct error, else it always
	 * return -1.
	 */
	if (ret > sizeof(msg->apiversion) && (msg->apiversion != PROGRESS_API_VERSION))
		return -EBADMSG;
	if (ret != sizeof(*msg)) {
		close(*connfd);
		*connfd = -1;
		return -1;
	}

	return ret;
}
