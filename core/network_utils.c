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

#include "bsdqueue.h"
#include "network_utils.h"
#include "util.h"

struct socket_meta {
  char *path;
  SIMPLEQ_ENTRY(socket_meta) next;
};

static pthread_mutex_t sockets_toclose_lock = PTHREAD_MUTEX_INITIALIZER;
SIMPLEQ_HEAD(self_sockets, socket_meta);
static struct self_sockets sockets_toclose;

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
