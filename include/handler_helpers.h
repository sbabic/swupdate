/*
 * (C) Copyright 2022
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include "swupdate_image.h"
struct chain_handler_data {
	struct img_type img;
};

#define FIFO_THREAD_READ	0
#define FIFO_HND_WRITE		1

struct hnd_load_priv {
	int fifo[2];	/* PIPE where to write */
	size_t	totalbytes;
	int exit_status;
};

struct bgtask_handle {
	const char *cmd;
	char *parms;
	struct img_type *img;
};

void *chain_handler_thread(void *data);
extern int handler_transfer_data(void *data, const void *buf, size_t len);
int bgtask_handler(struct bgtask_handle *bg);
