/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
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
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>

#include "swupdate.h"
#include <handler.h>
#include "util.h"
#include "network_ipc.h"
#include "network_interface.h"
#include <progress.h>

struct progress_conn {
	SIMPLEQ_ENTRY(progress_conn) next;
	int sockfd;
};

SIMPLEQ_HEAD(connections, progress_conn);

/*
 * Structure contains data regarding
 * current installation
 */
struct swupdate_progress {
	struct progress_msg msg;
	char *current_image;
	const handler *curhnd;
	struct connections conns;
	pthread_mutex_t lock;
	bool step_running;
};
static struct swupdate_progress progress;

/*
 * This must be called after acquiring the mutex
 * for the progress structure
 */
static void send_progress_msg(void)
{
	struct progress_conn *conn;
	struct swupdate_progress *prbar = &progress;
	void *buf;
	size_t count;
	ssize_t n;

	SIMPLEQ_FOREACH(conn, &prbar->conns, next) {
		buf = &prbar->msg;
		count = sizeof(prbar->msg);
		while (count > 0) {
			n = send(conn->sockfd, buf, count, MSG_NOSIGNAL);
			if (n <= 0) {
				TRACE("A progress client disappeared, removing it.");
				close(conn->sockfd);
				SIMPLEQ_REMOVE(&prbar->conns, conn,
					       	progress_conn, next);
				free(conn);
				break;
			}
			count -= (size_t)n;
			buf = (char*)buf + n;
		}
	}
}

void swupdate_progress_init(unsigned int nsteps) {
	struct swupdate_progress *prbar = &progress;
	pthread_mutex_lock(&prbar->lock);

	prbar->msg.nsteps = nsteps;
	prbar->msg.cur_step = 0;
	prbar->msg.status = START;
	prbar->msg.cur_percent = 0;
	prbar->msg.infolen = get_install_info(&prbar->msg.source, prbar->msg.info,
						sizeof(prbar->msg.info));
	send_progress_msg();
	pthread_mutex_unlock(&prbar->lock);
}

void swupdate_progress_update(unsigned int perc)
{
	struct swupdate_progress *prbar = &progress;
	pthread_mutex_lock(&prbar->lock);
	if (perc != prbar->msg.cur_percent && prbar->step_running) {
		prbar->msg.cur_percent = perc;
		send_progress_msg();
	}
	pthread_mutex_unlock(&prbar->lock);
}

void swupdate_progress_inc_step(char *image)
{
	struct swupdate_progress *prbar = &progress;
	pthread_mutex_lock(&prbar->lock);
	prbar->msg.cur_step++;
	prbar->msg.cur_percent = 0;
	strncpy(prbar->msg.cur_image, image, sizeof(prbar->msg.cur_image));
	prbar->step_running = true;
	prbar->msg.status = RUN;
	send_progress_msg();
	pthread_mutex_unlock(&prbar->lock);
}

void swupdate_progress_step_completed(void)
{
	struct swupdate_progress *prbar = &progress;
	pthread_mutex_lock(&prbar->lock);
	prbar->step_running = false;
	prbar->msg.status = IDLE;
	pthread_mutex_unlock(&prbar->lock);
}

void swupdate_progress_end(RECOVERY_STATUS status)
{
	struct swupdate_progress *prbar = &progress;
	pthread_mutex_lock(&prbar->lock);
	prbar->step_running = false;
	prbar->msg.status = status;
	send_progress_msg();
	pthread_mutex_unlock(&prbar->lock);
}

void swupdate_progress_done(void)
{
	swupdate_progress_end(DONE);
}

void *progress_bar_thread (void __attribute__ ((__unused__)) *data)
{
	int listen, connfd;
	socklen_t clilen;
	struct sockaddr_un cliaddr;
	struct swupdate_progress *prbar = &progress;
	struct progress_conn *conn;

	pthread_mutex_init(&prbar->lock, NULL);
	SIMPLEQ_INIT(&prbar->conns);

	/* Initialize and bind to UDS */
	listen = listener_create(SOCKET_PROGRESS_PATH, SOCK_STREAM);
	if (listen < 0 ) {
		TRACE("Error creating IPC sockets");
		exit(2);
	}

	do {
		clilen = sizeof(cliaddr);
		if ( (connfd = accept(listen, (struct sockaddr *) &cliaddr, &clilen)) < 0) {
			if (errno == EINTR)
				continue;
			else {
				TRACE("Accept returns: %s", strerror(errno));
				continue;
			}
		}

		/*
		 * Save the new connection to be handled by the progress thread
		 */
		conn = (struct progress_conn *)calloc(1, sizeof(*conn));
		if (!conn) {
			ERROR("Out of memory, skipping...");
			continue;
		}
		conn->sockfd = connfd;
		pthread_mutex_lock(&prbar->lock);
		SIMPLEQ_INSERT_TAIL(&prbar->conns, conn, next);
		pthread_mutex_unlock(&prbar->lock);
	} while(1);
}
