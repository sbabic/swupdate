/*
 * Author: Christian Storm
 * Copyright (C) 2017, Siemens AG
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <progress_ipc.h>
#if defined(__FreeBSD__)
#define ENODATA ENOATTR
#define ETIME ETIMEDOUT
#endif

#ifdef CONFIG_SOCKET_PROGRESS_PATH
char *SOCKET_PROGRESS_PATH = (char*)CONFIG_SOCKET_PROGRESS_PATH;
#else
char *SOCKET_PROGRESS_PATH = NULL;
#endif

#define SOCKET_PROGRESS_DEFAULT  "swupdateprog"

static inline int progress_is_major_version_compatible(unsigned int other_version)
{
	return  PROGRESS_API_MAJOR == ((other_version >> 16) & 0xFFFF);
}

char *get_prog_socket(void) {
	if (!SOCKET_PROGRESS_PATH || !strlen(SOCKET_PROGRESS_PATH)) {
		const char *socketdir = getenv("RUNTIME_DIRECTORY");
		if(!socketdir){
			socketdir = getenv("TMPDIR");
		}
		if (!socketdir) {
			if (access("/run/swupdate", W_OK) == 0)
				socketdir = "/run/swupdate";
			else
				socketdir = "/tmp";
		}
		if (asprintf(&SOCKET_PROGRESS_PATH, "%s/%s", socketdir, SOCKET_PROGRESS_DEFAULT) == -1)
			return (char *)"/tmp/"SOCKET_PROGRESS_DEFAULT;
	}

	return SOCKET_PROGRESS_PATH;
}

/* Decrease timeout depending on elapsed time */
static int update_timeout(const struct timespec *initial_time, int *timeout_ms)
{
	struct timespec current_time;
	int diff_timeout_ms;
	struct timespec elapsed;
	int err = clock_gettime(CLOCK_MONOTONIC, &current_time);
	if (err) {
		return -errno;
	}
	elapsed.tv_sec = current_time.tv_sec - initial_time->tv_sec;
	elapsed.tv_nsec = current_time.tv_nsec - initial_time->tv_nsec;

	diff_timeout_ms = *timeout_ms - (elapsed.tv_sec*1000 + elapsed.tv_nsec/1E6);

	*timeout_ms = diff_timeout_ms;
	return 0;
}

static int progress_ipc_recv_ack(int fd, struct progress_connect_ack *ack)
{
	struct timespec initial_time;
	int err;
	int ret = 0; /* 0 == success */
	int timeout_ms = 5000; /* 5 s should be enough in most cases as the socket is local */
	err = clock_gettime(CLOCK_MONOTONIC, &initial_time);
	if (err) {
		return -errno;
	}

	unsigned int size_to_read = sizeof(struct progress_connect_ack);
	unsigned int offset = 0;

	while (size_to_read > 0) {
		int err_poll;
		struct pollfd pfds[1];
		pfds[0].fd = fd;
		pfds[0].events = POLLIN;
		do {
			err_poll = poll(pfds, 1, timeout_ms);
			int err_update_timeout = update_timeout(&initial_time, &timeout_ms);
			if (err_update_timeout) return err_update_timeout;
		} while (err_poll < 0 && errno == EINTR);

		if (err_poll == -1) {
			ret = -errno;
			break;
		} else if (err_poll == 0) {
			/* Timeout */
			ret = -ETIME;
			break;
		} else if (pfds[0].revents & POLLHUP) {
			/* The peer closed its end of the channel */
			/* (note that some operating systems also set POLLIN in this case) */
			ret = -ECONNRESET;
			break;
		} else if (pfds[0].revents & POLLIN) {
			/* There is a message to read */
			int n = read(fd, (void*)ack + offset, size_to_read);
			if (n == -1 && errno == EINTR) {
				continue; /* redo poll() and timeout management */
			} else if (0 == n) {
				/* error, as at least 1 byte should be pending */
				ret = -ENODATA;
			} else if (n < 0) {
				/* read error */
				ret = -errno;
				break;
			}
			size_to_read -= n;
			offset += n;

		} else {
			/* unexpected error */
			ret = -EOPNOTSUPP;
		}
	}

	return ret;
}

/* Wait for the daemon to send an ACK
 *
 * Returns:
 *      0 success
 *     <0 error (timeout, peer closed, invalid ACK, ...)
 */
static int progress_ipc_wait_for_ack(int fd)
{
	struct progress_connect_ack ack;
	int err = progress_ipc_recv_ack(fd, &ack);
	if (err) {
		return err;
	}
	if (! progress_is_major_version_compatible(ack.apiversion)) {
		return -EBADMSG;
	}
	if (0 != strcmp(ack.magic, PROGRESS_CONNECT_ACK_MAGIC)) {
		return -EBADMSG;
	}
	return 0;
}

static int _progress_ipc_connect(const char *socketpath, bool reconnect)
{
	struct sockaddr_un servaddr;
	int fd = socket(AF_LOCAL, SOCK_STREAM, 0);
	int err;
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

	/* Connected. Wait for ACK */
	err = progress_ipc_wait_for_ack(fd);
	if (err < 0) {
		close(fd);
		return -1;
	}

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

int progress_ipc_receive_nb(int *connfd, struct progress_msg *msg) {
	int ret = -1;
	int err_poll;
	struct pollfd pfds[1];
	pfds[0].fd = *connfd;
	pfds[0].events = POLLIN;
	do {
		err_poll = poll(pfds, 1, 0);
	} while (err_poll == -1 && errno == EINTR);

	if (err_poll == -1) {
		/* poll error */
		ret = -1;
	} else if (err_poll == 0) {
		/* no pending message */
		ret = 0;
	} else if (pfds[0].revents & POLLIN) {
		/* there is a message to read or the peer closed its end of the channel */
		/* (some operating systems set POLLIN|POLLHUP on this later case) */
		ret = progress_ipc_receive(connfd, msg);
	} else if (pfds[0].revents & POLLHUP) {
		/* the peer closed its end of the channel */
		ret = -1;
	} else {
		/* unexpected error */
		ret = -1;
	}

	return ret;
}
