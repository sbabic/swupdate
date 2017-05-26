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

/* Suricatta Main Interface.
 *
 * `start_suricatta()` is the main interface to suricatta's functionality.
 * It's implementation defines the main loop comprising polling for updates
 * and installing them. For interoperability with different server and channel
 * implementations, the valid result codes to be returned by the different
 * implementations are defined here.
 */

typedef enum {
	CHANNEL_OK,
	CHANNEL_EINIT,
	CHANNEL_ENONET,
	CHANNEL_ENOMEM,
	CHANNEL_EACCES,
	CHANNEL_ENOENT,
	CHANNEL_EIO,
	CHANNEL_EILSEQ,
	CHANNEL_EAGAIN,
	CHANNEL_ELOOP,
	CHANNEL_EBADMSG
} channel_op_res_t;

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
