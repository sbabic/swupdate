/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <util.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include "pctl.h"
#include "suricatta/suricatta.h"
#include "suricatta/server.h"

int suricatta_wait(int seconds)
{
	fd_set readfds;
	struct timeval tv;
	int retval;

	tv.tv_sec = seconds;
	tv.tv_usec = 0;
	FD_ZERO(&readfds);
	FD_SET(sw_sockfd, &readfds);
	DEBUG("Sleeping for %ld seconds.\n", tv.tv_sec);
	retval = select(sw_sockfd + 1, &readfds, NULL, NULL, &tv);
	if (retval < 0) {
		TRACE("Suricatta awakened because of: %s\n", strerror(errno));
		return 0;
	}
	if (retval && FD_ISSET(sw_sockfd, &readfds)) {
		TRACE("Suricatta woke up for IPC at %ld seconds", tv.tv_sec);
		if (server.ipc(sw_sockfd) != SERVER_OK){
			DEBUG("Handling IPC failed!");
		}
		return (int)tv.tv_sec;
	}
	return 0;
}

int start_suricatta(const char *cfgfname, int argc, char *argv[])
{
	int action_id;
	sigset_t sigpipe_mask;
	sigset_t saved_mask;

	sigemptyset(&sigpipe_mask);
	sigaddset(&sigpipe_mask, SIGPIPE);
	sigprocmask(SIG_BLOCK, &sigpipe_mask, &saved_mask);

	if (server.start(cfgfname, argc, argv) != SERVER_OK) {
		exit(EXIT_FAILURE);
	}

	TRACE("Server initialized, entering suricatta main loop.\n");
	while (true) {
		switch (server.has_pending_action(&action_id)) {
		case SERVER_UPDATE_AVAILABLE:
			DEBUG("About to process available update.\n");
			server.install_update();
			break;
		case SERVER_ID_REQUESTED:
			server.send_target_data();
			break;
		case SERVER_EINIT:
			break;
		case SERVER_OK:
		default:
			DEBUG("No pending action to process.\n");
			break;
		}

		for (int wait_seconds = server.get_polling_interval();
			 wait_seconds > 0;
			 wait_seconds = min(wait_seconds, (int)server.get_polling_interval())) {
			wait_seconds = suricatta_wait(wait_seconds);
		}

		TRACE("Suricatta awakened.\n");
	}
}
