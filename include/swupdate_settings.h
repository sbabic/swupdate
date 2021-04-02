/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _SWUPDATE_SETTINGS_H
#define _SWUPDATE_SETTINGS_H

#include <unistd.h>

typedef int (*settings_callback)(void *elem, void *data);

#ifdef CONFIG_LIBCONFIG

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
#else

typedef struct {} swupdate_cfg_handle;

static inline void swupdate_cfg_init(swupdate_cfg_handle __attribute__ ((__unused__))*handle) { }

static inline int swupdate_cfg_read_file(swupdate_cfg_handle __attribute__ ((__unused__))*handle,
		const char __attribute__ ((__unused__))*filename)
{
	return -1;
}

static inline void swupdate_cfg_destroy(swupdate_cfg_handle __attribute__ ((__unused__))*handle) {
	return;
}

static inline int read_module_settings(swupdate_cfg_handle __attribute__ ((__unused__))*handle,
		const char __attribute__ ((__unused__))*module,
		settings_callback __attribute__ ((__unused__))fcn,
		void __attribute__ ((__unused__))*data) {
	return -1;
};

/*
 * Without LIBCONFIG, let run with current user
 */
static inline int read_settings_user_id(swupdate_cfg_handle __attribute__ ((__unused__))*handle,
					const char __attribute__ ((__unused__))*module,
					uid_t *userid, gid_t *groupid)
{
	*userid = getuid();
	*groupid = getgid();

	return 0;
}

static inline int settings_into_dict(void __attribute__ ((__unused__)) *settings,
					void __attribute__ ((__unused__))*data)
{
	return -1;
}
#endif

#endif
