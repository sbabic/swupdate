/*
 * (C) Copyright 2013-2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
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
static bool console_ansi_colors = false;

/*
 * Escape sequences:
 * they are in the format <ESC>[{attr};{fg};{bg}m
 * where :
 * <ESC> Escape char 0x1B
 * attr : attriibute
 * fg : foreground
 * bg : background
 */

enum console_attr {
	RESET,
	BRIGHT,
	DIM,
	UNDERLINE,
	BLINK,
	REVERSE,
	HIDDEN
};

#define RESET		0
#define BRIGHT 		1
#define DIM		2
#define UNDERLINE 	3
#define BLINK		4
#define REVERSE		7
#define HIDDEN		8



enum console_colors {
	BLACK,
	RED,
	GREEN,
	YELLOW,
	BLUE,
	MAGENTA,
	CYAN,
	WHITE,
	COLOR_NONE
};

struct logcolor {
	int attr;
	int fg;
	int bg;
};

#define RESET_COLOR "\x1b[0m"

static const char *ascii_string_colors[] = {
	"black",
	"red",
	"green",
	"yellow",
	"blue",
	"magenta",
	"cyan",
	"white",
	"none"
};

static const char *ascii_string_attributes[] = {
	"normal",
	"bright",
	"dim",
	"underline",
	"underline",
	"blink",
	"blink",
	"reverse",
	"hidden"
};

struct logcolor consolecolors[] = {
	[ERRORLEVEL] = {BRIGHT, RED, COLOR_NONE},
	[WARNLEVEL] = {BRIGHT, YELLOW, COLOR_NONE},
	[INFOLEVEL] = {BRIGHT, GREEN, COLOR_NONE},
	[DEBUGLEVEL] = {RESET, COLOR_NONE, COLOR_NONE},
	[TRACELEVEL] = {RESET, COLOR_NONE, COLOR_NONE}
};

static void set_console_color(int level, char *buf, size_t size) {
	struct logcolor *attr;
	if (level < 0 || level >= ARRAY_SIZE(consolecolors))
		return;
	memset(buf, 0, size);
	attr = &consolecolors[level];
	if (attr->fg == COLOR_NONE && attr->attr == RESET)
		return;
	if (attr->fg != COLOR_NONE)
		snprintf(buf, size, "%c[%d;%dm", 0x1B, attr->attr, attr->fg + 30);
	else
		snprintf(buf, size, "%c[%dm", 0x1B, attr->attr);
}

void notifier_set_color(int level, char *col)
{
	int i;
	char *attr;
	if (level < ERRORLEVEL || level > LASTLOGLEVEL || !col)
		return;
	attr = strchr(col, ':');
	if (attr && (strlen(col) > (attr - col + 1))) {
		*attr = '\0';
		attr++;
	} else
		attr = NULL;

	for (i = 0; i < ARRAY_SIZE(ascii_string_colors); i++) {
		if (!strcmp(col, ascii_string_colors[i])) {
			consolecolors[level].fg = i;
		}
	}
	if (attr)
		for (i = 0; i < ARRAY_SIZE(ascii_string_attributes); i++) {
			if (!strcmp(attr, ascii_string_attributes[i])) {
				consolecolors[level].attr = i;
			}
		}
}

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
				strlcpy(notifymsg.buf, msg, sizeof(notifymsg.buf) - 1);
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
	char color[32];
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
	/*
	 * PROGRESS is a special case. It is used for subprocesses to send
	 * progress information via the notifier. A trace with this status
	 * is processed by the progress notifier
	 */
	case PROGRESS:
		return;
	case DONE:
		strncpy(current, "SWUPDATE done : ", sizeof(current));
		break;
	}

	if (console_ansi_colors)
		set_console_color(level, color, sizeof(color));
	else
		color[0] = '\0';

	switch (level) {
	case ERRORLEVEL:
		fprintf(stderr, "%s%s[ERROR]", color,
				console_priority_prefix ? "<3>" : "");
		break;
	case WARNLEVEL:
		fprintf(stdout, "%s%s[WARN ]", color,
				console_priority_prefix ? "<4>" : "");
		break;
	case INFOLEVEL:
		fprintf(stdout, "%s%s[INFO ]", color,
				console_priority_prefix ? "<6>" : "");
		break;
	case DEBUGLEVEL:
		fprintf(stdout, "%s%s[DEBUG]", color,
				console_priority_prefix ? "<7>" : "");
		break;
	case TRACELEVEL:
		fprintf(stdout, "%s%s[TRACE]", color,
				console_priority_prefix ? "<7>" : "");
		break;
	}

	fprintf(level == ERRORLEVEL ? stderr : stdout,
			" : %s %s%s\n", current, msg ? msg : "", console_ansi_colors ? "\x1b[0m" : "");
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
 * Progress notifier: the message should be forwarded to the progress
 * interface only.
 */
static void progress_notifier (RECOVERY_STATUS status, int event, int level, const char *msg)
{
	int dwl_percent = 0;
	unsigned long long dwl_bytes = 0;
	(void)level;

	/* Check just in case a process want to send an info outside */
	if (status != PROGRESS)
	       return;

	if (event == RECOVERY_DWL && (sscanf(msg, "%d-%llu", &dwl_percent, &dwl_bytes) == 2)) {
		swupdate_download_update(dwl_percent, dwl_bytes);
		return;
	}

	swupdate_progress_info(status, event, msg);
}


#if defined(__FreeBSD__)
static char* socket_path = NULL;
static void unlink_socket(void)
{
	if (socket_path) {
		unlink(socket_path);
		free(socket_path);
	}
}

static void setup_socket_cleanup(struct sockaddr_un *addr)
{
	socket_path = strndup(addr->sun_path, sizeof(addr->sun_path));
	if (atexit(unlink_socket) != 0) {
		TRACE("Cannot setup socket cleanup on exit, %s won't be unlinked.", socket_path);
	}
	unlink(socket_path);
}
#endif

/*
 * Utility function to setup the internal IPC
 */
static void addr_init(struct sockaddr_un *addr, const char *path)
{
	memset(addr, 0, sizeof(struct sockaddr_un));
	addr->sun_family = AF_UNIX;
#if defined(__linux__)
	/*
	 * Use Linux-specific abstract sockets for this internal interface
	 */
	strcpy(&addr->sun_path[1], path);
	addr->sun_path[0] = '\0';
#elif defined(__FreeBSD__)
	/*
	 * Use filesystem-backed sockets on FreeBSD although this interface
	 * is internal as there are no Linux-like abstract sockets.
	 */
	strncpy(addr->sun_path, CONFIG_SOCKET_NOTIFIER_DIRECTORY, sizeof(addr->sun_path));
	strncat(addr->sun_path, path,
			sizeof(addr->sun_path)-strlen(CONFIG_SOCKET_NOTIFIER_DIRECTORY));
#else
	ERROR("Undetected OS, probably sockets won't function as expected.");
#endif
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
	int attempt = 0;
	struct notify_ipc_msg msg;

	/* Initialize and bind to UDS */
	serverfd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (serverfd < 0) {
		fprintf(stderr, "Error creating notifier daemon, exiting.");
		exit(2);
	}

#if defined(__FreeBSD__)
	setup_socket_cleanup(&notify_server);
#endif

	int len_socket_name = strlen(&notify_server.sun_path[1]);

	do {
		errno = 0;
		if (bind(serverfd, (const struct sockaddr *) &notify_server,
			sizeof(struct sockaddr_un)) < 0) {
			if (errno == EADDRINUSE && attempt < 10) {
				attempt++;
				/*
				 * Start increasing the socket as
				 * NotifyServer1, NotifyServer2...
				 */
				notify_server.sun_path[len_socket_name + 1] = '0' + attempt;
			} else {
				fprintf(stderr, "Error binding notifier socket: %s, exiting.\n", strerror(errno));
				close(serverfd);
				exit(2);
			}
		} else
			break;
	} while (1);

	thread_ready();
	do {
		len =  recvfrom(serverfd, &msg, sizeof(msg), 0, NULL, NULL);
		/*
		 * Force msg.buf to be Null Terminated
		 */
		msg.buf [sizeof(msg.buf) - 1] = '\0';

		if (len > 0) {
			notify(msg.status, msg.error, msg.level, msg.buf);
		}

	} while(1);
}

void notify_init(void)
{

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

	console_ansi_colors = (isatty(fileno(stdout)) && isatty(fileno(stderr)))
		? true : false;

	if (pid == getpid()) {
		char buf[60];
		snprintf(buf, sizeof(buf), "Notify%d", pid);
		addr_init(&notify_client, buf);
#if defined(__FreeBSD__)
		setup_socket_cleanup(&notify_client);
#endif
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
		/*
		 * If this is the main process, start setting the name of the
		 * socket. This can changed if more as one instance of swupdate
		 * is started (name adjusted to avoid adrress is in use)
		 */
		addr_init(&notify_server, "NotifyServer");
		STAILQ_INIT(&clients);
		register_notifier(console_notifier);
		register_notifier(process_notifier);
		register_notifier(progress_notifier);
		start_thread(notifier_thread, NULL);
	}
}
