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

#pragma once

/* Suricatta Server Interface.
 *
 * Each suricatta server has to implement this interface.
 * Cf. `server_hawkbit.c` for an example implementation targeted towards the
 * [hawkBit](https://projects.eclipse.org/projects/iot.hawkbit) server.
 */

extern server_op_res_t server_has_pending_action(int *action_id);
extern server_op_res_t server_install_update(void);
extern unsigned int server_get_polling_interval(void);
extern server_op_res_t server_start(char *cfgfname, int argc, char *argv[]);
extern server_op_res_t server_stop(void);

static struct server_t {
	server_op_res_t (*has_pending_action)(int *action_id);
	server_op_res_t (*install_update)(void);
	unsigned int (*get_polling_interval)(void);
	server_op_res_t (*start)(char *fname, int argc, char *argv[]);
	server_op_res_t (*stop)(void);
} server = {.has_pending_action = &server_has_pending_action,
	    .install_update = &server_install_update,
	    .get_polling_interval = &server_get_polling_interval,
	    .start = &server_start,
	    .stop = &server_stop};
