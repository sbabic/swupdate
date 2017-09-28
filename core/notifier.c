/*
 * (C) Copyright 2013-2016
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
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>

#include "bsdqueue.h"
#include "util.h"
#include "pctl.h"
#include "progress.h"

#ifdef CONFIG_SYSTEMD
#include <sys/stat.h>
#include <systemd/sd-daemon.h>
#endif

/*
 * There is a list of notifier. Each registered
 * notifier will receive the notification
 * and can process it.
 */
struct notify_elem {
	notifier client;
	STAILQ_ENTRY(notify_elem) next;
};

STAILQ_HEAD(notifylist, notify_elem);

static struct notifylist clients;

/*
 * Notification can be sent even by other
 * processes - if they are started by
 * SWUpdate.
 * It is checked if the notification is
 * coming from the main process - if not,
 * message is sent to an internal IPC
 */
struct notify_ipc_msg {
	RECOVERY_STATUS status;
	int error;
	int level;
	char buf[NOTIFY_BUF_SIZE];
};

static struct sockaddr_un notify_client;
static struct sockaddr_un notify_server;
static int notifyfd = -1;
static bool console_priority_prefix = false;

/*
 * This allows to extend the list of notifier.
 * One can register a new notifier and it will
 * receive any notification that is sent via
 * the notify() call
 */
int register_notifier(notifier client)
{

	struct notify_elem *newclient;

	if (!client)
		return -1;

	newclient = (struct notify_elem *)calloc(1, sizeof(struct notify_elem));
	newclient->client = client;

	STAILQ_INSERT_TAIL(&clients, newclient, next);

	return 0;
}

/*
 * Main function to send notification. It is checked
 * if it is sent by the main process, where the notifier
 * are running. If not, send the notification via
 * IPC to the main process that will dispatch it
 * to the notifiers.
 */
void notify(RECOVERY_STATUS status, int error, int level, const char *msg)
{
	struct notify_elem *elem;
	struct notify_ipc_msg notifymsg;

	if (pid == getpid()) {
		if (notifyfd > 0) {
			notifymsg.status = status;
			notifymsg.error = error;
			notifymsg.level = level;
			if (msg)
				strcpy(notifymsg.buf, msg);
			else
				notifymsg.buf[0] = '\0';
			sendto(notifyfd, &notifymsg, sizeof(notifymsg), 0,
			      (struct sockaddr *) &notify_server,
				sizeof(struct sockaddr_un));
		}
	} else { /* Main process */
		STAILQ_FOREACH(elem, &clients, next)
			(elem->client)(status, error, level, msg);
	}
}

/*
 * Default notifier, it prints to stdout
 */
static void console_notifier (RECOVERY_STATUS status, int error, int level, const char *msg)
{
	char current[80];
	switch(status) {
	case IDLE:
		strncpy(current, "No SWUPDATE running : ", sizeof(current));
		break;
	case DOWNLOAD:
		strncpy(current, "SWUPDATE downloading : ", sizeof(current));
		break;
	case START:
		strncpy(current, "SWUPDATE started : ", sizeof(current));
		break;
	case RUN:
		strncpy(current, "SWUPDATE running : ", sizeof(current));
		break;
	case SUCCESS:
		strncpy(current, "SWUPDATE successful !", sizeof(current));
		break;
	case FAILURE:
		snprintf(current, sizeof(current), "SWUPDATE failed [%d]", error);
		break;
	case SUBPROCESS:
		snprintf(current, sizeof(current), "EVENT [%d] : ", error );
		break;
	case DONE:
		strncpy(current, "SWUPDATE done : ", sizeof(current));
		break;
	}

	switch (level) {
	case ERRORLEVEL:
		fprintf(stderr, "%s[ERROR]", console_priority_prefix ? "<3>" : "");
		break;
	case WARNLEVEL:
		fprintf(stdout, "%s[WARN ]", console_priority_prefix ? "<4>" : "");
		break;
	case INFOLEVEL:
		fprintf(stdout, "%s[INFO ]", console_priority_prefix ? "<6>" : "");
		break;
	case DEBUGLEVEL:
		fprintf(stdout, "%s[DEBUG]", console_priority_prefix ? "<7>" : "");
		break;
	case TRACELEVEL:
		fprintf(stdout, "%s[TRACE]", console_priority_prefix ? "<7>" : "");
		break;
	}

	fprintf(level == ERRORLEVEL ? stderr : stdout,
			" : %s %s\n", current, msg ? msg : "");
	fflush(stdout);
}

/*
 * Process notifier: this is called when a process has something to say
 * and wants that the information is passed to the progress interface
 */
static void process_notifier (RECOVERY_STATUS status, int event, int level, const char *msg)
{
	(void)level;

	/* Check just in case a process want to send an info outside */
	if (status != SUBPROCESS)
	       return;

	switch (event) {
	case (CANCELUPDATE):
		status = FAILURE;
		break;
	}

	swupdate_progress_info(status, event, msg);

}

/*
 * Utility function to setup the internal IPC
 */
static void addr_init(struct sockaddr_un *addr, const char *path)
{
	memset(addr, 0, sizeof(struct sockaddr_un));
	addr->sun_family = AF_UNIX;
	strcpy(&addr->sun_path[1], path);
	addr->sun_path[0] = 0;
}

/*
 * Notifier thread: it runs in the context of the main
 * process.
 * This allows to have a central point to manage
 * all logs.
 */
static void *notifier_thread (void __attribute__ ((__unused__)) *data)
{
	int serverfd;
	int len;
	struct notify_ipc_msg msg;

	/* Initialize and bind to UDS */
	serverfd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (serverfd < 0) {
		TRACE("Error creating notifier daemon");
		exit(2);
	}
	memset(&notify_server, 0, sizeof(notify_server));
	notify_server.sun_family = AF_UNIX;
	strcpy(notify_server.sun_path, "#NotifyServer");

	/*
	 *  Use Abstract Socket Address because this is an internal interface
	 */
	notify_server.sun_path[0] = 0;

	if (bind(serverfd, (const struct sockaddr *) &notify_server,
			sizeof(struct sockaddr_un)) < 0) {
		close(serverfd);
		TRACE("Error bind notifier socket");
		exit(2);
	}

	do {
		len =  recvfrom(serverfd, &msg, sizeof(msg), 0, NULL, NULL);

		if (len > 0) {
			notify(msg.status, msg.error, msg.level, msg.buf);
		}

	} while(1);
}

void notify_init(void)
{
	addr_init(&notify_server, "NotifyServer");

#ifdef CONFIG_SYSTEMD
	/*
	 * If the init system is systemd and SWUpdate is run as
	 * systemd service, then prefix the console log messages
	 * with priority values enclosed in < >, following the
	 * scheme used by the kernel's printk().
	 * These get picked up and are interpreted by journald.
	 * systemd >= 231 (2016-07-25) is required for proper
	 * detection of stdout/stderr being attached to journald.
	 */
	if (sd_booted() && getenv("JOURNAL_STREAM") != NULL) {
		dev_t device;
		ino_t inode;
		if (sscanf(getenv("JOURNAL_STREAM"), "%lu:%lu", &device, &inode) == 2) {
			struct stat statbuffer;
			if (fstat(fileno(stderr), &statbuffer) == 0) {
				if (inode == statbuffer.st_ino && device == statbuffer.st_dev) {
					console_priority_prefix = true;
				}
			}
		}
	}
#endif

	if (pid == getpid()) {
		char buf[60];
		snprintf(buf, sizeof(buf), "Notify%d", pid);
		addr_init(&notify_client, buf);
		notifyfd = socket(AF_UNIX, SOCK_DGRAM, 0);
		if (notifyfd < 0) {
			printf("Error creating notifier socket for pid %d", pid);
			return;
		}
		if (bind(notifyfd, (const struct sockaddr *) &notify_client,
			sizeof(struct sockaddr_un)) < 0) {
				/* Trace cannot work here, use printf */
				fprintf(stderr, "Cannot initialize notification for pid %d\n",
					pid);
			close(notifyfd);
			return;
		}
	} else {
		STAILQ_INIT(&clients);
		register_notifier(console_notifier);
		register_notifier(process_notifier);
		start_thread(notifier_thread, NULL);
	}
}
