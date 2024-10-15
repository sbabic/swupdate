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
#include "handler_helpers.h"
#include "installer.h"
#include "pctl.h"
#include "util.h"

/*
 * This is the copyimage's callback. When called,
 * there is a buffer to be passed to curl connections
 * This is part of the image load: using copyimage() the image is
 * transferred without copy to the daemon.
 */
int handler_transfer_data(void *data, const void *buf, size_t len)
{
	struct hnd_load_priv *priv = (struct hnd_load_priv *)data;
	ssize_t written;
	unsigned int nbytes = len;
	const void *tmp = buf;

	while (nbytes) {
		written = write(priv->fifo[FIFO_HND_WRITE], buf, len);
		if (written < 0) {
			ERROR ("Cannot write to fifo");
			return -EFAULT;
		}
		nbytes -= written;
		tmp += written;
	}

	return 0;
}


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


