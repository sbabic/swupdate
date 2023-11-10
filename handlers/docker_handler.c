/*
 * (C) Copyright 2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * This handler communicates with the docker socket via REST API
 * to install new images and manage containers.
 */

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <handler.h>
#include <pthread.h>
#include <util.h>
#include <signal.h>
#include <json-c/json.h>
#include "parselib.h"
#include "swupdate_image.h"
#include "docker_interface.h"

void docker_loadimage_handler(void);
void docker_deleteimage_handler(void);
void docker_pruneimage_handler(void);
void docker_createcontainer_handler(void);
void docker_deletecontainer_handler(void);
void docker_container_start_handler(void);
void docker_container_stop_handler(void);

typedef server_op_res_t (*docker_fn)(const char *name);

#define FIFO_THREAD_READ	0
#define FIFO_HND_WRITE		1

struct hnd_load_priv {
	int fifo[2];	/* PIPE where to write */
	size_t	totalbytes;
	int exit_status;
};

/*
 * This is the copyimage's callback. When called,
 * there is a buffer to be passed to curl connections
 * This is part of the image load: using copyimage() the image is
 * transferred without copy to the daemon.
 */
static int transfer_data(void *data, const void *buf, size_t len)
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
 * Background threa dto transfer the image to the daemon.
 * main thread ==> run copyimage
 * backgound thread ==> push the incoming data to ddaemon
 */
static void *curl_transfer_thread(void *p)
{
	struct hnd_load_priv *hnd = (struct hnd_load_priv *)p;

	hnd->exit_status = docker_image_load(hnd->fifo[FIFO_THREAD_READ], hnd->totalbytes);

	close(hnd->fifo[FIFO_THREAD_READ]);

	pthread_exit(NULL);
}

/*
 * Implementation /images/load
 */
static int docker_install_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	struct hnd_load_priv priv;
	pthread_attr_t attr;
	pthread_t transfer_thread;
	int thread_ret;
	int ret = 0;
	ssize_t bytes;

	signal(SIGPIPE, SIG_IGN);

	/*
	 * Requires the size of file to be transferrred
	 */
	bytes = get_output_size(img, true);
	if (bytes < 0) {
		ERROR("Size to be uploaded undefined");
		return -EINVAL;
	}

	priv.totalbytes = bytes;

	/*
	 * Create one FIFO for each connection to be thread safe
	 */
	if (pipe(priv.fifo) < 0) {
		ERROR("Cannot create internal pipes, exit..");
		ret = FAILURE;
		goto handler_exit;
	}
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	thread_ret = pthread_create(&transfer_thread, &attr, curl_transfer_thread, &priv);
	if (thread_ret) {
		ERROR("Code from pthread_create() is %d",
			thread_ret);
			transfer_thread = 0;
			ret = FAILURE;
			goto handler_exit;
	}

	ret = copyimage(&priv, img, transfer_data);
	if (ret) {
		ERROR("Transferring SWU image was not successful");
		ret = FAILURE;
		goto handler_exit;
	}

	void *status;
	ret = pthread_join(transfer_thread, &status);

	close(priv.fifo[FIFO_HND_WRITE]);

	return priv.exit_status;

handler_exit:
	return ret;
}

/*
 * Implementation POST /container/create
 */
static int docker_create_container(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	struct script_handler_data *script_data = data;
	char *script = NULL;
	char *buf = NULL;
	struct stat sb;
	int result = 0;
	int fd = -1;

	/*
	 * Call only in case of postinstall
	 */
	if (!script_data || script_data->scriptfn != POSTINSTALL)
		return 0;


	if (asprintf(&script, "%s%s", get_tmpdirscripts(), img->fname) == ENOMEM_ASPRINTF) {
		ERROR("OOM when creating script path");
		return -ENOMEM;
	}

	if (stat(script, &sb) == -1) {
		ERROR("stat fails on %s", script);
		result = -EFAULT;
		goto create_container_exit;
	}

	fd = open(script, O_RDONLY);
	if (fd < 0) {
		ERROR("%s cannot be opened, exiting..", script);
		result = -EFAULT;
		goto create_container_exit;
	}

	buf = (char *)malloc(sb.st_size);
	if (!buf) {
		ERROR("OOM creating buffer for reading %s of %ld bytes",
		      script, sb.st_size);
		result =  -ENOMEM;
		goto create_container_exit;
	}

	ssize_t n = read(fd, buf, sb.st_size);
	if (n != sb.st_size) {
		ERROR("Script %s cannot be read, return value %ld != %ld",
		      script, n, sb.st_size);
		result = -EFAULT;
		goto create_container_exit;
	}

	char *name = dict_get_value(&img->properties, "name");

	TRACE("DOCKER CREATE CONTAINER");

	result = docker_container_create(name, buf);

create_container_exit:
	free(script);
	free(buf);
	if (fd > 0) close(fd);

	return result;

}

/*
 * Implementation DELETE /container/{id}
 */
static int docker_query(struct img_type *img, void *data, docker_fn fn)
{
	struct script_handler_data *script_data = data;
	/*
	 * Call only in case of postinstall
	 */
	if (!script_data || script_data->scriptfn != POSTINSTALL)
		return 0;

	char *name = dict_get_value(&img->properties, "name");

	if (!name) {
		ERROR("DELETE container: name is missing, it is mandatory");
		return -EINVAL;
	}

	return fn(name);
}

static int container_delete(struct img_type *img, void *data)
{
	return docker_query(img, data, docker_container_remove);
}

static int image_delete(struct img_type *img, void *data)
{
	return docker_query(img, data, docker_image_remove);
}

static int image_prune(struct img_type *img, void *data)
{
	return docker_query(img, data, docker_image_prune);
}


static int container_start(struct img_type *img, void *data)
{
	return docker_query(img, data, docker_container_start);
}


static int container_stop(struct img_type *img, void *data)
{
	return docker_query(img, data, docker_container_stop);
}

__attribute__((constructor))
void docker_loadimage_handler(void)
{
	register_handler("docker_imageload", docker_install_image,
				IMAGE_HANDLER, NULL);
}

__attribute__((constructor))
void docker_deleteimage_handler(void)
{
	register_handler("docker_imagedelete", image_delete,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}

__attribute__((constructor))
void docker_pruneimage_handler(void)
{
	register_handler("docker_imageprune", image_prune,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}


__attribute__((constructor))
void docker_createcontainer_handler(void)
{
	register_handler("docker_containercreate", docker_create_container,
				SCRIPT_HANDLER, NULL);
}

__attribute__((constructor))
void docker_deletecontainer_handler(void)
{
	register_handler("docker_containerdelete", container_delete,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}

__attribute__((constructor))
void docker_container_start_handler(void)
{
	register_handler("docker_containerstart", container_start,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}

__attribute__((constructor))
void docker_container_stop_handler(void)
{
	register_handler("docker_containerstart", container_stop,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}
