/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#pragma once

#include "channel_op_res.h"

/* Suricatta Main Interface.
 *
 * `start_suricatta()` is the main interface to suricatta's functionality.
 * It's implementation defines the main loop comprising polling for updates
 * and installing them. For interoperability with different server and channel
 * implementations, the valid result codes to be returned by the different
 * implementations are defined here.
 */

typedef enum {
	SERVER_OK,
	SERVER_EERR,
	SERVER_EBADMSG,
	SERVER_EINIT,
	SERVER_EACCES,
	SERVER_EAGAIN,
	SERVER_UPDATE_AVAILABLE,
	SERVER_NO_UPDATE_AVAILABLE,
	SERVER_UPDATE_CANCELED,
	SERVER_ID_REQUESTED,
} server_op_res_t;

int start_suricatta(const char *cfgname, int argc, char *argv[]) __attribute__((noreturn));
void suricatta_print_help(void);
int suricatta_wait(int seconds);
