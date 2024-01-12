/*
 * (C) Copyright 2016
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
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
#include "pctl.h"
#include "network_ipc.h"
#include "network_utils.h"
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
 * It is assumed that the listeners are reading
 * the events and consume the messages. To avoid deadlocks,
 * SWUpdate will try to send the message without blocking,
 * and if send() returns EWOULDBLOCk, tries some times again
 * with a 1 second delay.
 * This is because SWUpdate could send a lot of events,
 * and listeners cannot be scheduled at time. The delay just
 * slow down the update when happens. If the message cannot be
 * sent after retries, SWUpdate will consider the listener
 * dead and removes it from the list.
 */
static void send_progress_msg(void)
{
	struct progress_conn *conn, *tmp;
	struct swupdate_progress *pprog = &progress;
	void *buf;
	size_t count;
	ssize_t n;
	bool tryagain;
	const int maxAttempts = 5;

	pprog->msg.apiversion = PROGRESS_API_VERSION;
	SIMPLEQ_FOREACH_SAFE(conn, &pprog->conns, next, tmp) {
		buf = &pprog->msg;
		count = sizeof(pprog->msg);
		errno = 0;
		pprog->msg.source = get_install_source();
		while (count > 0) {
			int attempt = 0;
			do {
				n = send(conn->sockfd, buf, count, MSG_NOSIGNAL | MSG_DONTWAIT);
				attempt++;
				tryagain = n <= 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
				if (tryagain)
					sleep(1);
			} while (tryagain && attempt < maxAttempts);
			if (n <= 0) {
				close(conn->sockfd);
				SIMPLEQ_REMOVE(&pprog->conns, conn,
					       	progress_conn, next);
				free(conn);
				break;
			}
			count -= (size_t)n;
			buf = (char*)buf + n;
		}
	}
}

static void _swupdate_download_update(unsigned int perc, unsigned long long totalbytes)
{
	/*
	 * TODO: totalbytes should be forwarded correctly
	 * after adding it to the progress message
	 */
	struct swupdate_progress *pprog = &progress;
	pthread_mutex_lock(&pprog->lock);
	if (perc != pprog->msg.dwl_percent) {
		pprog->msg.status = DOWNLOAD;
		pprog->msg.dwl_percent = perc;
		pprog->msg.dwl_bytes = totalbytes;
		send_progress_msg();
	}
	pthread_mutex_unlock(&pprog->lock);
}

void swupdate_progress_init(unsigned int nsteps) {
	struct swupdate_progress *pprog = &progress;
	pthread_mutex_lock(&pprog->lock);

	pprog->msg.apiversion = PROGRESS_API_VERSION;
	pprog->msg.nsteps = nsteps;
	pprog->msg.cur_step = 0;
	pprog->msg.status = START;
		pprog->msg.cur_percent = 0;
	pprog->msg.infolen = get_install_info(pprog->msg.info,
						sizeof(pprog->msg.info));
	pprog->msg.source = get_install_source();
	send_progress_msg();
	/* Info is just an event, reset it after sending */
	pprog->msg.infolen = 0;
	pthread_mutex_unlock(&pprog->lock);
}

void swupdate_progress_addstep(void) {
	struct swupdate_progress *pprog = &progress;
	pthread_mutex_lock(&pprog->lock);
	pprog->msg.nsteps++;
	pthread_mutex_unlock(&pprog->lock);
}

void swupdate_progress_update(unsigned int perc)
{
	struct swupdate_progress *pprog = &progress;
	pthread_mutex_lock(&pprog->lock);
	if (perc != pprog->msg.cur_percent && pprog->step_running) {
		pprog->msg.status = PROGRESS;
		pprog->msg.cur_percent = perc;
		send_progress_msg();
	}
	pthread_mutex_unlock(&pprog->lock);
}

void swupdate_download_update(unsigned int perc, unsigned long long totalbytes)
{
	char	info[PRINFOSIZE];   		/* info */

	/*
	 * Not called by main process, for example by suricatta or Webserver
	 */
	if (pid == getpid()) {
		/*
		 * Notify can just receive a string as message
		 * so it is necessary to encode further information as string
		 * and decode them in the notifier, in this case
		 * the progress_notifier
		 */
		snprintf(info, sizeof(info) - 1, "%d-%llu", perc, totalbytes);
		notify(PROGRESS, RECOVERY_DWL, TRACELEVEL, info);
		return;
	}

	/* Called by main process, emit a progress message */
	_swupdate_download_update(perc, totalbytes);
}

void swupdate_progress_inc_step(const char *image, const char *handler_name)
{
	struct swupdate_progress *pprog = &progress;
	pthread_mutex_lock(&pprog->lock);
	pprog->msg.cur_step++;
	pprog->msg.cur_percent = 0;
	strlcpy(pprog->msg.cur_image, image, sizeof(pprog->msg.cur_image));
	strlcpy(pprog->msg.hnd_name, handler_name, sizeof(pprog->msg.hnd_name));
	pprog->step_running = true;
	pprog->msg.status = RUN;
	send_progress_msg();
	pthread_mutex_unlock(&pprog->lock);
}

void swupdate_progress_step_completed(void)
{
	struct swupdate_progress *pprog = &progress;
	pthread_mutex_lock(&pprog->lock);
	pprog->step_running = false;
	pprog->msg.status = IDLE;
	pthread_mutex_unlock(&pprog->lock);
}

void swupdate_progress_end(RECOVERY_STATUS status)
{
	struct swupdate_progress *pprog = &progress;
	pthread_mutex_lock(&pprog->lock);
	pprog->step_running = false;
	pprog->msg.status = status;
	send_progress_msg();
	pprog->msg.nsteps = 0;
	pprog->msg.cur_step = 0;
	pprog->msg.cur_percent = 0;
	pprog->msg.dwl_percent = 0;
	pprog->msg.dwl_bytes = 0;

	pthread_mutex_unlock(&pprog->lock);
}

void swupdate_progress_info(RECOVERY_STATUS status, int cause, const char *info)
{
	struct swupdate_progress *pprog = &progress;
	pthread_mutex_lock(&pprog->lock);
	snprintf(pprog->msg.info, sizeof(pprog->msg.info), "{\"%d\": %s}",
			cause, info);
	pprog->msg.infolen = strlen(pprog->msg.info);
	pprog->msg.status = status;
	send_progress_msg();
	/* Info is just an event, reset it after sending */
	pprog->msg.infolen = 0;
	pthread_mutex_unlock(&pprog->lock);
}

void swupdate_progress_done(const char *info)
{
	struct swupdate_progress *pprog = &progress;
	pthread_mutex_lock(&pprog->lock);
	if (info != NULL) {
		snprintf(pprog->msg.info, sizeof(pprog->msg.info), "%s", info);
		pprog->msg.infolen = strlen(pprog->msg.info);
	}
	pprog->step_running = false;
	pprog->msg.status = DONE;
	send_progress_msg();
	pprog->msg.infolen = 0;
	pthread_mutex_unlock(&pprog->lock);
}

void *progress_bar_thread (void __attribute__ ((__unused__)) *data)
{
	int listen, connfd;
	socklen_t clilen;
	struct sockaddr_un cliaddr;
	struct swupdate_progress *pprog = &progress;
	struct progress_conn *conn;

	pthread_mutex_init(&pprog->lock, NULL);
	SIMPLEQ_INIT(&pprog->conns);

	/* Initialize and bind to UDS */
	listen = listener_create(get_prog_socket(), SOCK_STREAM);
	if (listen < 0 ) {
		ERROR("Error creating IPC socket %s, exiting.", get_prog_socket());
		exit(2);
	}

	thread_ready();
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

		if (fcntl(connfd, F_SETFD, FD_CLOEXEC) < 0)
			WARN("Could not set %d as cloexec: %s", connfd, strerror(errno));

		/*
		 * Save the new connection to be handled by the progress thread
		 */
		conn = (struct progress_conn *)calloc(1, sizeof(*conn));
		if (!conn) {
			ERROR("Out of memory, skipping...");
			continue;
		}
		conn->sockfd = connfd;
		pthread_mutex_lock(&pprog->lock);
		SIMPLEQ_INSERT_TAIL(&pprog->conns, conn, next);
		pthread_mutex_unlock(&pprog->lock);
	} while(1);
}
