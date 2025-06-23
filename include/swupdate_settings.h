/*
 * (C) Copyright 2016-2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <unistd.h>

typedef int (*settings_callback)(void *elem, void *data);

#include <libconfig.h>

typedef struct {
	config_t cfg;
} swupdate_cfg_handle;

void swupdate_cfg_init(swupdate_cfg_handle *handle);
int swupdate_cfg_read_file(swupdate_cfg_handle *handle, const char *filename);

void swupdate_cfg_destroy(swupdate_cfg_handle *handle);
int read_module_settings(swupdate_cfg_handle *handle, const char *module, settings_callback fcn, void *data);
int read_settings_user_id(swupdate_cfg_handle *handle, const char *module, uid_t *userid, gid_t *groupid);
int settings_into_dict(void *settings, void *data);
