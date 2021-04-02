/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <network_ipc.h>
#include "util.h"

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
extern server_op_res_t server_ipc(ipc_message *msg);
extern void server_print_help(void);

static struct server_t {
	server_op_res_t (*has_pending_action)(int *action_id);
	server_op_res_t (*install_update)(void);
	server_op_res_t (*send_target_data)(void);
	unsigned int (*get_polling_interval)(void);
	server_op_res_t (*start)(const char *fname, int argc, char *argv[]);
	server_op_res_t (*stop)(void);
	server_op_res_t (*ipc)(ipc_message *msg);
	void (*help)(void);
} server = {.has_pending_action = &server_has_pending_action,
	    .install_update = &server_install_update,
	    .send_target_data = &server_send_target_data,
	    .get_polling_interval = &server_get_polling_interval,
	    .start = &server_start,
	    .stop = &server_stop,
	    .ipc = &server_ipc,
	    .help = &server_print_help,
};
