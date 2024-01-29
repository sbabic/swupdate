/*
 * (C) Copyright 2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include "server_utils.h"

typedef enum {
	DOCKER_IMAGE_LOAD,
	DOCKER_IMAGE_DELETE,
	DOCKER_IMAGE_PRUNE,
	DOCKER_CONTAINER_CREATE,
	DOCKER_CONTAINER_DELETE,
	DOCKER_CONTAINER_START,
	DOCKER_CONTAINER_STOP,
	DOCKER_VOLUMES_CREATE,
	DOCKER_VOLUMES_DELETE,
	DOCKER_NETWORKS_CREATE,
	DOCKER_NETWORKS_DELETE,
	DOCKER_SERVICE_LAST = DOCKER_NETWORKS_DELETE,
} docker_services_t;

typedef server_op_res_t (*docker_fn)(const char *name, const char *setup);
docker_fn docker_fn_lookup(docker_services_t service);
server_op_res_t docker_image_load(int fd, size_t nbytes);
