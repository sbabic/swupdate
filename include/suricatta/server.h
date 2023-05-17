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
typedef struct {
	server_op_res_t (*has_pending_action)(int *action_id);
	server_op_res_t (*install_update)(void);
	server_op_res_t (*send_target_data)(void);
	unsigned int (*get_polling_interval)(void);
	server_op_res_t (*start)(const char *fname, int argc, char *argv[]);
	server_op_res_t (*stop)(void);
	server_op_res_t (*ipc)(ipc_message *msg);
	void (*help)(void);
} server_t;

bool register_server(const char *name, server_t *server);
