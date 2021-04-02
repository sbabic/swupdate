/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>

#include "swupdate.h"
#include "handler.h"
#include "util.h"

void dummy_handler(void);

static int install_nothing(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	int ret;
	int fdout;

	if (img->is_partitioner | img->is_script)
		return 0;

	fdout = open("/dev/null", O_WRONLY);
	if (fdout < 0) {
		TRACE("Device %s cannot be opened: %s",
				"/dev/null", strerror(errno));
		return -1;
	}

	ret = copyimage(&fdout, img, NULL);

	close(fdout);
	return ret;
}

__attribute__((constructor))
void dummy_handler(void)
{
	register_handler("dummy", install_nothing,
				IMAGE_HANDLER |
				FILE_HANDLER |
				SCRIPT_HANDLER |
				PARTITION_HANDLER,
				NULL);
}
