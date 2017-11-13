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
typedef struct channel channel_t;
struct channel {
	channel_op_res_t (*open)(channel_t *this, void *cfg);
	channel_op_res_t (*close)(channel_t *this);
	channel_op_res_t (*get)(channel_t *this, void *data);
	channel_op_res_t (*get_file)(channel_t *this, void *data, int file_handle);
	channel_op_res_t (*put)(channel_t *this, void *data);
	void *priv;
};

channel_t *channel_new(void);
