/*
 * Copyright (C) 2024
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include "handler_helpers.h"
#include "installer.h"
#include "swupdate_image.h"
#include "pctl.h"
#include "util.h"

struct thread_handle {
	struct hnd_load_priv *hndtransfer;
	struct img_type *img;
};

static void *copyimage_thread(void *p)
{
	struct thread_handle *hnd = (struct thread_handle *)p;
	struct hnd_load_priv *priv = hnd->hndtransfer;
	int ret = 0;

	ret = copyimage(priv, hnd->img, handler_transfer_data);

	if (ret) {
		ERROR("Transferring image was not successful");
	}
	close(priv->fifo[FIFO_HND_WRITE]);
	pthread_exit((void*)(intptr_t)ret);
}

/*
 * This is the copyimage's callback. When called,
 * there is a buffer to be passed to curl connections
 * This is part of the image load: using copyimage() the image is
 * transferred without copy to the daemon.
 */
int handler_transfer_data(void *data, const void *buf, size_t len)
{
	struct hnd_load_priv *priv = (struct hnd_load_priv *)data;
	size_t nbytes = len;
	const char *tmp = buf;

	while (nbytes > 0) {
		ssize_t written;
		written = write(priv->fifo[FIFO_HND_WRITE], tmp, nbytes);
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

/*
 * This receive a command from a handler as external command that must be executed.
 * It performs the following:
 * - creates pipes for internal IPC
 * - creates a thread that run (fork) the command
 * - execute copyimage with callback that write into the FIFO
 * - wait until thread exits and returns the result
 */
int bgtask_handler(struct bgtask_handle *bg)
{
	int ret = 0;
	struct hnd_load_priv data_handle;
	struct thread_handle hnd;
	char *cmd;

	/*
	 * if no path for the command is found, the handler assumes that
	 * btrfs is in standard path
	 */
	if (!bg->cmd || !bg->img)
		return -EINVAL;

	if (access(bg->cmd, X_OK)) {
		ERROR("Handler requires %s, not found.", bg->cmd);
		return -EINVAL;
	}

	/*
	 * Create one FIFO for each connection to be thread safe
	 */
	if (pipe(data_handle.fifo) < 0) {
		ERROR("Cannot create internal pipes, exit..");
		return -EFAULT;
	}

	hnd.hndtransfer = &data_handle;
	hnd.img = bg->img;
	cmd = swupdate_strcat(3, bg->cmd, " ", bg->parms);

	/*
	 * Starts a backgroud task to fill in the
	 * FIFO using copyimage
	 */
	start_thread(copyimage_thread, &hnd);

	/*
	 * Start to write in the FIFO - vene if the bg process is not started.
	 * If the FIFO becomes full, this thread will be paused until the bg
	 * process (and btrfs receive) will start to consume the data
	 */

	ret = run_system_cmd_with_fdin(cmd, data_handle.fifo);
	free(cmd);
	return ret;
}

static int generic_executor(struct img_type *img,
		      void __attribute__ ((__unused__)) *data)
{
	int ret = 0;
	struct bgtask_handle handle;
	const char *cmd = dict_get_value(&img->properties, "cmd");

	if (!cmd) {
		ERROR("No cmd set, add cmd property");
		return -EINVAL;
	}

	handle.cmd = cmd;
	handle.parms = dict_get_value(&img->properties, "parms");
	handle.img = img;

	ret = bgtask_handler(&handle);

	return ret;
}

__attribute__((constructor))
static void executor_handler(void)
{
	register_handler("executor", generic_executor,
				IMAGE_HANDLER, NULL);
}
