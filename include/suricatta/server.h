/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
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
extern server_op_res_t server_send_target_data(void);
extern unsigned int server_get_polling_interval(void);
extern server_op_res_t server_start(const char *cfgfname, int argc, char *argv[]);
extern server_op_res_t server_stop(void);
extern server_op_res_t server_ipc(int fd);
extern void server_print_help(void);

static struct server_t {
	server_op_res_t (*has_pending_action)(int *action_id);
	server_op_res_t (*install_update)(void);
	server_op_res_t (*send_target_data)(void);
	unsigned int (*get_polling_interval)(void);
	server_op_res_t (*start)(const char *fname, int argc, char *argv[]);
	server_op_res_t (*stop)(void);
	server_op_res_t (*ipc)(int fd);
	void (*help)(void);
} server = {.has_pending_action = &server_has_pending_action,
	    .install_update = &server_install_update,
	    .send_target_data = &server_send_target_data,
	    .get_polling_interval = &server_get_polling_interval,
	    .help = &server_print_help,
	    .ipc = &server_ipc,
	    .start = &server_start,
	    .stop = &server_stop};
