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
#include "suricatta/suricatta.h"
#include "suricatta/server.h"

void start_suricatta(int argc, char *argv[])
{
	int action_id;
	if (server.start(argc, argv) != SERVER_OK) {
		exit(EXIT_FAILURE);
	}
	TRACE("Server initialized, entering suricatta main loop.\n");
	while (true) {
		switch (server.has_pending_action(&action_id)) {
		case SERVER_UPDATE_AVAILABLE:
			DEBUG("About to process available update.\n");
			server.install_update();
			break;
		case SERVER_EINIT:
			break;
		case SERVER_OK:
		default:
			DEBUG("No pending action to process.\n");
			break;
		}
		DEBUG("Sleeping for %d seconds.\n",
		      server.get_polling_interval());
		sleep(server.get_polling_interval());
		TRACE("Suricatta awakened.\n");
	}
}
