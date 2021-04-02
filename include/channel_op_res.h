/*
 * (C) Copyright 2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
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
	CHANNEL_EBADMSG,
	CHANNEL_ENOTFOUND,
	CHANNEL_EREDIRECT
} channel_op_res_t;
