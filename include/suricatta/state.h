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
#include <stdbool.h>
#include "suricatta.h"

/* (Persistent) Update State Management Functions.
 *
 * Suricatta may persistently store the update status to communicate it to the
 * server instance after, e.g., a successful reboot into the new firmware. The
 * `{save,read,reset}_state()` functions are called by a server implementation
 * to persistently manage the update state via, e.g., U-Boot's environment.
 */

typedef enum {
	STATE_OK = '0',
	STATE_INSTALLED = '1',
	STATE_TESTING = '2',
	STATE_FAILED = '3',
	STATE_NOT_AVAILABLE = '4',
	STATE_ERROR = '5'
} update_state_t;

server_op_res_t save_state(char *key, update_state_t value);
server_op_res_t read_state(char *key, update_state_t *value);
server_op_res_t reset_state(char *key);
bool is_state_valid(update_state_t state);
