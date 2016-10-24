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
#include "suricatta.h"

/* Suricatta Channel Interface.
 *
 * Each suricatta channel has to implement this interface.
 * Cf. `channel_hawkbit.c` for an example implementation targeted towards the
 * [hawkBit](https://projects.eclipse.org/projects/iot.hawkbit) server.
 */

extern channel_op_res_t channel_open(void);
extern channel_op_res_t channel_close(void);
extern channel_op_res_t channel_post(void *data);
extern channel_op_res_t channel_get(void *data);
extern channel_op_res_t channel_get_file(void *data);

static struct channel_t {
	channel_op_res_t (*open)(void);
	channel_op_res_t (*close)(void);
	channel_op_res_t (*get)(void *data);
	channel_op_res_t (*get_file)(void *data);
	channel_op_res_t (*post)(void *data);
} channel = {.open = &channel_open,
	     .close = &channel_close,
	     .get = &channel_get,
	     .get_file = &channel_get_file,
	     .post = &channel_post};
