/*
 * (C) Copyright 2012-2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
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

#include "bsdqueue.h"
#include "util.h"
#include "network_ipc.h"
#include "network_interface.h"
#include "installer.h"
#include "installer_priv.h"
#include "swupdate.h"
#include "pctl.h"
#include "generated/autoconf.h"
#include "state.h"

#ifdef CONFIG_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#define LISTENQ	1024

#define NUM_CACHED_MESSAGES 100
#define DEFAULT_INTERNAL_TIMEOUT 60

struct msg_elem {
	RECOVERY_STATUS status;
	int error;
	int level;
	char *msg;
	SIMPLEQ_ENTRY(msg_elem) next;
};

SIMPLEQ_HEAD(msglist, msg_elem);
static struct msglist notifymsgs;
static unsigned long nrmsgs = 0;

static pthread_mutex_t msglock = PTHREAD_MUTEX_INITIALIZER;

static bool is_selection_allowed(const char *software_set, char *running_mode,
				 struct dict const *acceptedlist)
{
	char *swset = NULL;
	struct dict_list *sets;
	struct dict_list_elem *selection;
	bool allowed = false;

	/*
	 * No attempt to change software set
	 */
	if (!strlen(software_set) || !strlen(running_mode))
		return true;

	if (ENOMEM_ASPRINTF ==
		asprintf(&swset, "%s,%s", software_set, running_mode)) {
			 ERROR("OOM generating selection string");
			 return false;
	}
	sets = dict_get_list((struct dict *)acceptedlist, "accepted");
	if (sets && swset) {
		LIST_FOREACH(selection, sets, next) {
			if (!strcmp(swset, selection->value)) {
				allowed = true;
			}
		}
		free(swset);
	}

	if (allowed) {
		INFO("Accepted selection %s,%s", software_set, running_mode);
	}else 
		ERROR("Selection %s,%s is not allowed, rejected !",
		      software_set, running_mode); 
	return allowed;
}

static void clean_msg(char *msg, char drop)
{
	char *lfpos;
	lfpos = strchr(msg, drop);
	while (lfpos) {
		*lfpos = ' ';
		lfpos = strchr(msg, drop);
	}
}

static void network_notifier(RECOVERY_STATUS status, int error, int level, const char *msg)
{
	int len = msg ? strlen(msg) : 0;
	struct msg_elem *newmsg = (struct msg_elem *)calloc(1, sizeof(*newmsg) + len + 1);
	struct msg_elem *oldmsg;

	if (!newmsg)
		return;

	pthread_mutex_lock(&msglock);
	nrmsgs++;
	if (nrmsgs > NUM_CACHED_MESSAGES) {
		oldmsg = SIMPLEQ_FIRST(&notifymsgs);
		SIMPLEQ_REMOVE_HEAD(&notifymsgs, next);
		free(oldmsg);
		nrmsgs--;
	}
	newmsg->msg = (char *)newmsg + sizeof(struct msg_elem);

	newmsg->status = status;
	newmsg->error = error;
	newmsg->level = level;

	if (msg) {
		strncpy(newmsg->msg, msg, len);
		clean_msg(newmsg->msg, '\t');
		clean_msg(newmsg->msg, '\n');
		clean_msg(newmsg->msg, '\r');
	}


	SIMPLEQ_INSERT_TAIL(&notifymsgs, newmsg, next);
	pthread_mutex_unlock(&msglock);
}

int listener_create(const char *path, int type)
{
	struct sockaddr_un servaddr;
	int listenfd = -1;

#ifdef CONFIG_SYSTEMD
	if (sd_booted()) {
		for (int fd = SD_LISTEN_FDS_START; fd < SD_LISTEN_FDS_START + sd_listen_fds(0); fd++) {
			if (sd_is_socket_unix(fd, SOCK_STREAM, 1, path, 0)) {
				listenfd = fd;
				break;
			}
		}
		if (listenfd == -1) {
			TRACE("got no socket at %s from systemd", path);
		} else {
			TRACE("got socket fd=%d at %s from systemd", listenfd, path);
		}
	}
#endif

	if (listenfd == -1) {
		TRACE("creating socket at %s", path);
		listenfd = socket(AF_LOCAL, type, 0);
		if (listenfd < 0) {
			return -1;
		}
		unlink(path);
		bzero(&servaddr, sizeof(servaddr));
		servaddr.sun_family = AF_LOCAL;
		strlcpy(servaddr.sun_path, path, sizeof(servaddr.sun_path) - 1);

		if (bind(listenfd,  (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0) {
			close(listenfd);
			return -1;
		}

		if (chmod(path,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) < 0)
			WARN("chmod cannot be set on socket, error %s", strerror(errno));
	}

	if (type == SOCK_STREAM)
		if (listen(listenfd, LISTENQ) < 0) {
			close(listenfd);
			return -1;
		}
	return listenfd;
}

static void cleanum_msg_list(void)
{
	struct msg_elem *notification;

	pthread_mutex_lock(&msglock);

	while (!SIMPLEQ_EMPTY(&notifymsgs)) {
		notification = SIMPLEQ_FIRST(&notifymsgs);
		SIMPLEQ_REMOVE_HEAD(&notifymsgs, next);
		free(notification);
	}
	nrmsgs = 0;
	pthread_mutex_unlock(&msglock);
}

static void empty_pipe(int fd)
{
	fd_set fds;
	int ret;
	ipc_message msg;
	struct timeval tv;

	do {
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		tv.tv_sec = 0;
		tv.tv_usec = 10000; /* Check for not more as 10 mSec */
		ret = select(fd + 1, &fds, NULL, NULL, &tv);
		if (ret <= 0 || !FD_ISSET(fd, &fds))
			break;
		/*
		 * simply read to drop old messages
		 */
		if (read(fd, &msg, sizeof(msg)) < 0)
			break;
	} while (1);
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
	unlink(get_ctrl_socket());
}

void *network_thread (void *data)
{
	struct installer *instp = (struct installer *)data;
	int ctrllisten, ctrlconnfd;
	socklen_t clilen;
	struct sockaddr_un cliaddr;
	ipc_message msg;
	int nread;
	struct msg_elem *notification;
	int ret;
	int pipe;
	fd_set pipefds;
	struct timeval tv;
	update_state_t value;

	if (!instp) {
		TRACE("Fatal error: Network thread aborting...");
		return (void *)0;
	}

	SIMPLEQ_INIT(&notifymsgs);
	register_notifier(network_notifier);

	/* Initialize and bind to UDS */
	ctrllisten = listener_create(get_ctrl_socket(), SOCK_STREAM);
	if (ctrllisten < 0 ) {
		TRACE("Error creating IPC sockets");
		exit(2);
	}

	if (atexit(unlink_socket) != 0) {
		TRACE("Cannot setup socket cleanup on exit, %s won't be unlinked.",
			  get_ctrl_socket());
	}

	do {
		clilen = sizeof(cliaddr);
		if ( (ctrlconnfd = accept(ctrllisten, (struct sockaddr *) &cliaddr, &clilen)) < 0) {
			if (errno == EINTR)
				continue;
			else {
				TRACE("Accept returns: %s", strerror(errno));
				continue;
			}
		}
		nread = read(ctrlconnfd, (void *)&msg, sizeof(msg));

		if (nread != sizeof(msg)) {
			TRACE("IPC message too short: fragmentation not supported");
			close(ctrlconnfd);
			continue;
		}
#ifdef DEBUG_IPC
		TRACE("request header: magic[0x%08X] type[0x%08X]", msg.magic, msg.type);
#endif

		pthread_mutex_lock(&stream_mutex);
		if (msg.magic == IPC_MAGIC)  {
			switch (msg.type) {
			case POST_UPDATE:
				if (postupdate(get_swupdate_cfg(),
							   msg.data.procmsg.len > 0 ? msg.data.procmsg.buf : NULL) == 0) {
					msg.type = ACK;
					sprintf(msg.data.msg, "Post-update actions successfully executed.");
				} else {
					msg.type = NACK;
					sprintf(msg.data.msg, "Post-update actions failed.");
				}
				break;
			case SWUPDATE_SUBPROCESS:
				/*
				 *  this request is not for the installer,
				 *  but for one of the subprocesses
				 *  forward the request without checking
				 *  the payload
				 */

				pipe = pctl_getfd_from_type(msg.data.procmsg.source);
				if (pipe < 0) {
					ERROR("Cannot find channel for requested process");
					msg.type = NACK;
					break;
				}
				TRACE("Received Message for %s",
					pctl_getname_from_type(msg.data.procmsg.source));
				if (fcntl(pipe, F_GETFL) < 0 && errno == EBADF) {
					ERROR("Pipe not available or closed: %d", pipe);
					msg.type = NACK;
					break;
				}

				/*
				 * Cleanup the queue to be sure there are not
				 * outstanding messages
				 */
				empty_pipe(pipe);

				ret = write(pipe, &msg, sizeof(msg));
				if (ret != sizeof(msg)) {
					ERROR("Writing to pipe failed !");
					msg.type = NACK;
					break;
				}

				/*
				 * Do not block forever for an answer
				 * This would block the whole thread
				 * If a message requires more time,
				 * the destination process should sent an
				 * answer back explaining this in the payload
				 */
				FD_ZERO(&pipefds);
				FD_SET(pipe, &pipefds);
				tv.tv_usec = 0;
				if (!msg.data.procmsg.timeout)
					tv.tv_sec = DEFAULT_INTERNAL_TIMEOUT;
				else
					tv.tv_sec = msg.data.procmsg.timeout;
				ret = select(pipe + 1, &pipefds, NULL, NULL, &tv);

				/*
				 * If there is an error or timeout,
				 * send a NACK back
				 */
				if (ret <= 0 || !FD_ISSET(pipe, &pipefds)) {
					msg.type = NACK;
					break;
				}

				ret = read(pipe, &msg, sizeof(msg));
				if (ret != sizeof(msg)) {
					ERROR("Reading from pipe failed !");
					msg.type = NACK;
					break;
				}

				/*
				 * ACK/NACK was inserted by the called SUBPROCESS
				 * It should not be touched here
				 */

				break;
			case REQ_INSTALL:
				TRACE("Incoming network request: processing...");
				if (instp->status == IDLE) {
					instp->fd = ctrlconnfd;
					instp->req = msg.data.instmsg.req;
					if (is_selection_allowed(instp->req.software_set,
								 instp->req.running_mode,
								 &instp->software->accepted_set)) {
						/*
						 * Prepare answer
						 */
						msg.type = ACK;

						/* Drop all old notification from last run */
						cleanum_msg_list();

						/* Wake-up the installer */
						pthread_cond_signal(&stream_wkup);
					} else
						msg.type = NACK;

				} else {
					msg.type = NACK;
					sprintf(msg.data.msg, "Installation in progress");
				}
				break;
			case GET_STATUS:
				msg.type = GET_STATUS;
				memset(msg.data.msg, 0, sizeof(msg.data.msg));
				msg.data.status.current = instp->status;
				msg.data.status.last_result = instp->last_install;
				msg.data.status.error = instp->last_error;

				/* Get first notification from the queue */
				pthread_mutex_lock(&msglock);
				notification = SIMPLEQ_FIRST(&notifymsgs);
				if (notification) {
					SIMPLEQ_REMOVE_HEAD(&notifymsgs, next);
					nrmsgs--;
					strncpy(msg.data.status.desc, notification->msg,
						sizeof(msg.data.status.desc) - 1);
#ifdef DEBUG_IPC
					printf("GET STATUS: %s\n", msg.data.status.desc);
#endif
					msg.data.status.current = notification->status;
					msg.data.status.error = notification->error;
				}
				pthread_mutex_unlock(&msglock);

				break;
			case SET_AES_KEY:
#ifndef CONFIG_PKCS11
				msg.type = ACK;
				if (set_aes_key(msg.data.aeskeymsg.key_ascii, msg.data.aeskeymsg.ivt_ascii))
#endif
					msg.type = NACK;
				break;
			case SET_UPDATE_STATE:
				value = *(update_state_t *)msg.data.msg;
				msg.type = (is_valid_state(value) &&
					    save_state((char *)STATE_KEY, value) == SERVER_OK)
					       ? ACK
					       : NACK;
				break;
			default:
				msg.type = NACK;
			}
		} else {
			/* Wrong request */
			msg.type = NACK;
			sprintf(msg.data.msg, "Wrong request: aborting");
		}
		ret = write(ctrlconnfd, &msg, sizeof(msg));
		if (ret < 0)
			printf("Error write on socket ctrl");

		if (msg.type != ACK)
			close(ctrlconnfd);
		pthread_mutex_unlock(&stream_mutex);
	} while (1);
	return (void *)0;
}
