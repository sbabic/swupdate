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
#include <sys/select.h>
#include "pctl.h"
#include "suricatta/suricatta.h"
#include "suricatta/server.h"

int start_suricatta(const char *cfgfname, int argc, char *argv[])
{
	int action_id;
	int retval;
	fd_set readfds;
	struct timeval tv;
	int nsecs;
	bool ipc_wakeup = false;

	if (server.start(cfgfname, argc, argv) != SERVER_OK) {
		exit(EXIT_FAILURE);
	}

	TRACE("Server initialized, entering suricatta main loop.\n");
	while (true) {
		if (ipc_wakeup) {
			server.ipc(sw_sockfd);
			nsecs = min((int)tv.tv_sec, server.get_polling_interval());
		} else {
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
			nsecs = server.get_polling_interval();

		}
		DEBUG("Sleeping for %d seconds.\n", nsecs);
		/*
		 * Fill in with IPC descriptor
		 * */
		ipc_wakeup = false;
		tv.tv_sec = nsecs;
		tv.tv_usec = 0;
		FD_ZERO(&readfds);
		FD_SET(sw_sockfd, &readfds);
		retval = select(sw_sockfd + 1, &readfds, NULL, NULL, &tv);
		if (retval < 0)
			continue;
		/*
		 * Check IPC communication
		 */
		if (retval && FD_ISSET(sw_sockfd, &readfds)) {
			ipc_wakeup = true;
			TRACE("Waked-up IPC %ld", tv.tv_sec);
		}

		TRACE("Suricatta awakened.\n");
	}
}
