/*
 * (C) Copyright 2022
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include "chained_handler.h"
#include "installer.h"
#include "pctl.h"
#include "util.h"

/*
 * Thread to start the chained handler.
 * This received from FIFO the reassembled stream with
 * the artifact and can pass it to the handler responsible for the install.
 */
void *chain_handler_thread(void *data)
{
	struct chain_handler_data *priv = (struct chain_handler_data *)data;
	struct img_type *img = &priv->img;
	unsigned long ret;

	thread_ready();
	if (img->fdin < 0) {
		return (void *)1;
	}

	img->install_directly = true;
	ret = install_single_image(img, false);

	if (ret) {
		ERROR("Chain handler return with Error");
		close(img->fdin);
	}

	return (void *)ret;
}


