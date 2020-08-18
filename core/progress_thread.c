/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
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
#include "generated/autoconf.h"

#ifdef CONFIG_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

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
	struct progress_conn *conn, *tmp;
	struct swupdate_progress *prbar = &progress;
	void *buf;
	size_t count;
	ssize_t n;

	SIMPLEQ_FOREACH_SAFE(conn, &prbar->conns, next, tmp) {
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
	/* Info is just an event, reset it after sending */
	prbar->msg.infolen = 0;
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

void swupdate_progress_inc_step(char *image, char *handler_name)
{
	struct swupdate_progress *prbar = &progress;
	pthread_mutex_lock(&prbar->lock);
	prbar->msg.cur_step++;
	prbar->msg.cur_percent = 0;
	strlcpy(prbar->msg.cur_image, image, sizeof(prbar->msg.cur_image));
	strlcpy(prbar->msg.hnd_name, handler_name, sizeof(prbar->msg.hnd_name));
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

void swupdate_progress_info(RECOVERY_STATUS status, int cause, const char *info)
{
	struct swupdate_progress *prbar = &progress;
	pthread_mutex_lock(&prbar->lock);
	snprintf(prbar->msg.info, sizeof(prbar->msg.info), "{'%d': %s}",
			cause, info);
	prbar->msg.infolen = strlen(prbar->msg.info);
	prbar->msg.status = status;
	send_progress_msg();
	/* Info is just an event, reset it after sending */
	prbar->msg.infolen = 0;
	pthread_mutex_unlock(&prbar->lock);
}

void swupdate_progress_done(const char *info)
{
	struct swupdate_progress *prbar = &progress;
	pthread_mutex_lock(&prbar->lock);
	if (info != NULL) {
		snprintf(prbar->msg.info, sizeof(prbar->msg.info), "%s", info);
		prbar->msg.infolen = strlen(prbar->msg.info);
	}
	prbar->step_running = false;
	prbar->msg.status = DONE;
	send_progress_msg();
	prbar->msg.infolen = 0;
	pthread_mutex_unlock(&prbar->lock);
}

static void unlink_socket(void)
{
#ifdef CONFIG_SYSTEMD
	if (sd_booted() && sd_listen_fds(0) > 0) {
		/*
		 * There were socket fds handed-over by systemd,
		 * so don't delete the socket file.
		 */
		return;
	}
#endif
	unlink(get_prog_socket());
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
	listen = listener_create(get_prog_socket(), SOCK_STREAM);
	if (listen < 0 ) {
		ERROR("Error creating IPC socket %s, exiting.", get_prog_socket());
		exit(2);
	}

	if (atexit(unlink_socket) != 0) {
		TRACE("Cannot setup socket cleanup on exit, %s won't be unlinked.",
			get_prog_socket());
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
