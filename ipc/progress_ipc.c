/*
 * Author: Christian Storm
 * Copyright (C) 2017, Siemens AG
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

#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>

#include <progress_ipc.h>

#ifdef CONFIG_SOCKET_PROGRESS_PATH
char* SOCKET_PROGRESS_PATH = (char*)CONFIG_SOCKET_PROGRESS_PATH;
#else
char* SOCKET_PROGRESS_PATH = (char*)"/tmp/swupdateprog";
#endif

int progress_ipc_connect(bool reconnect)
{
	struct sockaddr_un servaddr;
	int fd = socket(AF_LOCAL, SOCK_STREAM, 0);
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sun_family = AF_LOCAL;
	strcpy(servaddr.sun_path, SOCKET_PROGRESS_PATH);

	fprintf(stdout, "Trying to connect to SWUpdate...\n");

	do {
		if (connect(fd, (struct sockaddr *) &servaddr, sizeof(servaddr)) == 0) {
			break;
		}
		if (!reconnect) {
			fprintf(stderr, "cannot communicate with SWUpdate via %s\n", SOCKET_PROGRESS_PATH);
			exit(1);
		}

		usleep(10000);
	} while (true);

	fprintf(stdout, "Connected to SWUpdate via %s\n", SOCKET_PROGRESS_PATH);
	return fd;
}

int progress_ipc_receive(int *connfd, struct progress_msg *msg) {
	int ret = read(*connfd, msg, sizeof(*msg));
	if (ret != sizeof(*msg)) {
		fprintf(stdout, "Connection closing..\n");
		close(*connfd);
		*connfd = -1;
		return -1;
	}
	return ret;
}
