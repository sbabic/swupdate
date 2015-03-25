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
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>

#include "util.h"
#include "network_ipc.h"

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
		TRACE("Connect return with failure %s", strerror(errno));
		close(connfd);
		return ret;
	}

	return connfd;
}

int ipc_get_status(ipc_message *msg)
{
	int ret;
	int connfd;

	connfd = prepare_ipc();
	if (connfd < 0) {
		return -1;
	}
	memset(msg, 0, sizeof(*msg));
	msg->magic = IPC_MAGIC;
	msg->type = GET_STATUS;
	ret = write(connfd, msg, sizeof(*msg));
	if (ret != sizeof(*msg)) {
		TRACE("IPC Error");
		close(connfd);
		return -1;
	}
	ret = read(connfd, msg, sizeof(*msg));
	if (ret <= 0) {
		TRACE("No answer from server");
		close(connfd);
		return -1;
	}

	close(connfd);
	return 0;
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
		TRACE("No answer from server");
		close(connfd);
		return -1;
	}

	if (msg.type != ACK) {
		TRACE("Installation not accepted %s", msg.data.msg);
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
