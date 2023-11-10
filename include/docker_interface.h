/*
 * (C) Copyright 2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include "server_utils.h"

/* Load an image
 * fd : file descriptor where to read the stream to be pushed
 */
server_op_res_t docker_image_load(int fd, size_t nbytes);
server_op_res_t docker_image_remove(const char *name);
server_op_res_t docker_image_prune(const char *name);
server_op_res_t docker_container_create(const char *name, char *setup);
server_op_res_t docker_container_remove(const char *name);
server_op_res_t docker_container_start(const char *name);
server_op_res_t docker_container_stop(const char *name);
