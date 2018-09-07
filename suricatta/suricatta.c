/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
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
	DEBUG("Sleeping for %ld seconds.", tv.tv_sec);
	retval = select(sw_sockfd + 1, &readfds, NULL, NULL, &tv);
	if (retval < 0) {
		TRACE("Suricatta awakened because of: %s", strerror(errno));
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

	TRACE("Server initialized, entering suricatta main loop.");
	while (true) {
		switch (server.has_pending_action(&action_id)) {
		case SERVER_UPDATE_AVAILABLE:
			DEBUG("About to process available update.");
			server.install_update();
			break;
		case SERVER_ID_REQUESTED:
			server.send_target_data();
			break;
		case SERVER_EINIT:
			break;
		case SERVER_OK:
		default:
			DEBUG("No pending action to process.");
			break;
		}

		for (int wait_seconds = server.get_polling_interval();
			 wait_seconds > 0;
			 wait_seconds = min(wait_seconds, (int)server.get_polling_interval())) {
			wait_seconds = suricatta_wait(wait_seconds);
		}

		TRACE("Suricatta awakened.");
	}
}
