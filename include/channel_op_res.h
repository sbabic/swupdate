/*
 * (C) Copyright 2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
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
