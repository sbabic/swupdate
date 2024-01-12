/*
 * (C) Copyright 2017
 * Stefano Babic, stefano.babic@swupdate.org.
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
	CHANNEL_EREDIRECT,
	CHANNEL_ESSLCERT,
	CHANNEL_ESSLCONNECT,
	CHANNEL_REQUEST_PENDING,
} channel_op_res_t;
