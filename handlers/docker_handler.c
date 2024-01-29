/*
 * (C) Copyright 2017
 * Stefano Babic, stefano.babic@swupdate.org.
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
 * Docker API requires one parameter and maybe a JSON file used as configuration.
 * Pass to the docker client the properties (only name is checked) and a JSON file if
 * present. This is the script itself and not an "artifact image".
 */
static int docker_send_cmd_with_setup(struct img_type *img, void *data, docker_services_t service)
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
		goto send_to_docker_exit;
	}

	/*
	 * Load the script / JSON config in memory
	 */
	fd = open(script, O_RDONLY);
	if (fd < 0) {
		ERROR("%s cannot be opened, exiting..", script);
		result = -EFAULT;
		goto send_to_docker_exit;
	}

	buf = (char *)malloc(sb.st_size);
	if (!buf) {
		ERROR("OOM creating buffer for reading %s of %ld bytes",
		      script, sb.st_size);
		result =  -ENOMEM;
		goto send_to_docker_exit;
	}

	ssize_t n = read(fd, buf, sb.st_size);
	if (n != sb.st_size) {
		ERROR("Script %s cannot be read, return value %ld != %ld",
		      script, n, sb.st_size);
		result = -EFAULT;
		goto send_to_docker_exit;
	}

	/*
	 * Check for a "name" properties - this is mandatory
	 * when a resource is deleted
	 */
	char *name = dict_get_value(&img->properties, "name");

	/*
	 * Retrieve which function is responsible for a service
	 */
	docker_fn fn = docker_fn_lookup(service);

	/*
	 * Call docker internal client
	 */
	if (fn) {
		result = fn(name, buf);
	} else {
		result = -EINVAL;
		ERROR("Service %d not supported", service);
	}

send_to_docker_exit:
	/* cleanup and exit */
	free(script);
	free(buf);
	if (fd > 0) close(fd);

	return result;

}

/*
 * Simple service without configuration file
 * Just lokup for the client function and call it
 */
static int docker_query(struct img_type *img, void *data, docker_services_t service)
{
	struct script_handler_data *script_data = data;
	docker_fn fn;

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

	fn = docker_fn_lookup(service);

	if (!fn) {
		ERROR("Docker service %d nbot supported", service);
		return -EINVAL;
	}
	return fn(name, NULL);
}

/* Docker service wrappers */
static int container_create(struct img_type *img, void *data)
{
	return docker_send_cmd_with_setup(img, data, DOCKER_CONTAINER_CREATE);
}

static int container_delete(struct img_type *img, void *data)
{
	return docker_query(img, data, DOCKER_CONTAINER_DELETE);
}

static int image_delete(struct img_type *img, void *data)
{
	return docker_query(img, data, DOCKER_IMAGE_DELETE);
}

static int image_prune(struct img_type *img, void *data)
{
	return docker_query(img, data, DOCKER_IMAGE_PRUNE);
}

static int container_start(struct img_type *img, void *data)
{
	return docker_query(img, data, DOCKER_CONTAINER_START);
}

static int network_create(struct img_type *img, void *data)
{
	return docker_send_cmd_with_setup(img, data, DOCKER_NETWORKS_CREATE);
}

static int network_delete(struct img_type *img, void *data)
{
	return docker_query(img, data, DOCKER_NETWORKS_DELETE);
}

static int volume_create(struct img_type *img, void *data)
{
	return docker_send_cmd_with_setup(img, data, DOCKER_VOLUMES_CREATE);
}

static int volume_delete(struct img_type *img, void *data)
{
	return docker_query(img, data, DOCKER_VOLUMES_DELETE);
}


static int container_stop(struct img_type *img, void *data)
{
	return docker_query(img, data, DOCKER_CONTAINER_STOP);
}

/* Handlers entry points */

__attribute__((constructor))
static void docker_loadimage_handler(void)
{
	register_handler("docker_imageload", docker_install_image,
				IMAGE_HANDLER, NULL);
}

__attribute__((constructor))
static void docker_deleteimage_handler(void)
{
	register_handler("docker_imagedelete", image_delete,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}

__attribute__((constructor))
static void docker_pruneimage_handler(void)
{
	register_handler("docker_imageprune", image_prune,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}


__attribute__((constructor))
static void docker_createcontainer_handler(void)
{
	register_handler("docker_containercreate", container_create,
				SCRIPT_HANDLER, NULL);
}

__attribute__((constructor))
static void docker_deletecontainer_handler(void)
{
	register_handler("docker_containerdelete", container_delete,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}

__attribute__((constructor))
static void docker_container_start_handler(void)
{
	register_handler("docker_containerstart", container_start,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}

__attribute__((constructor))
static void docker_container_stop_handler(void)
{
	register_handler("docker_containerstart", container_stop,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}

__attribute__((constructor))
static void docker_createnetwork_handler(void)
{
	register_handler("docker_networkcreate", network_create,
				SCRIPT_HANDLER, NULL);
}

__attribute__((constructor))
static void docker_deletenetwork_handler(void)
{
	register_handler("docker_networkdelete", network_delete,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}

__attribute__((constructor))
static void docker_createvolume_handler(void)
{
	register_handler("docker_volumecreate", volume_create,
				SCRIPT_HANDLER, NULL);
}

__attribute__((constructor))
static void docker_deletevolume_handler(void)
{
	register_handler("docker_volumedelete", volume_delete,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}
