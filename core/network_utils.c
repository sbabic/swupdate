/*
 * (C) Copyright 2023
 * Felix Moessbauer <felix.moessbauer@siemens.com>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/socket.h>

#ifdef CONFIG_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include "bsdqueue.h"
#include "network_utils.h"
#include "util.h"

#define LISTENQ	1024

struct socket_meta {
  char *path;
  SIMPLEQ_ENTRY(socket_meta) next;
};

static pthread_mutex_t sockets_toclose_lock = PTHREAD_MUTEX_INITIALIZER;
SIMPLEQ_HEAD(self_sockets, socket_meta);
static struct self_sockets sockets_toclose;


int listener_create(const char *path, int type)
{
	struct sockaddr_un servaddr;
	int listenfd = -1;

#ifdef CONFIG_SYSTEMD
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
		if(register_socket_unlink(path) != 0){
			ERROR("Out of memory, skipping...");
			return -1;
		}
		if (bind(listenfd,  (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0) {
			close(listenfd);
			return -1;
		}

		if (chmod(path,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) < 0)
			WARN("chmod cannot be set on socket, error %s", strerror(errno));
	}

	if (fcntl(listenfd, F_SETFD, FD_CLOEXEC) < 0)
		WARN("Could not set %d as cloexec: %s", listenfd, strerror(errno));

	if (type == SOCK_STREAM)
		if (listen(listenfd, LISTENQ) < 0) {
			close(listenfd);
			return -1;
		}
	return listenfd;
}

int register_socket_unlink(const char* path){
	struct socket_meta *socketm = malloc(sizeof(*socketm));
	if(!socketm){
		return -1;
	}
	socketm->path = strdup(path);
	if(!socketm->path){
		free(socketm);
		return -1;
	}
	pthread_mutex_lock(&sockets_toclose_lock);
	SIMPLEQ_INSERT_TAIL(&sockets_toclose, socketm, next);
	pthread_mutex_unlock(&sockets_toclose_lock);
	return 0;
}

static void unlink_sockets(void)
{
	pthread_mutex_lock(&sockets_toclose_lock);
	while(!SIMPLEQ_EMPTY(&sockets_toclose)) {
			struct socket_meta *socketm;
			socketm = SIMPLEQ_FIRST(&sockets_toclose);
			TRACE("unlink socket %s", socketm->path);
			unlink(socketm->path);
			free(socketm->path);
			SIMPLEQ_REMOVE_HEAD(&sockets_toclose, next);
			free(socketm);
	}
	pthread_mutex_unlock(&sockets_toclose_lock);
}

int init_socket_unlink_handler(void){
    SIMPLEQ_INIT(&sockets_toclose);
    return atexit(unlink_sockets);
}
