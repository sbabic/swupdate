/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#ifndef _SWUPDATE_SETTINGS_H
#define _SWUPDATE_SETTINGS_H

#ifdef CONFIG_LIBCONFIG
int read_module_settings(const char *filename, const char *module, settings_callback fcn, void *data);
int read_settings_user_id(const char *filename, const char *module, uid_t *userid, gid_t *groupid);
int settings_into_dict(void *settings, void *data);
#else
#include <unistd.h>
static inline int read_module_settings(const char __attribute__ ((__unused__))*filename,
		const char __attribute__ ((__unused__)) *module,
		settings_callback __attribute__ ((__unused__)) fcn,
		void __attribute__ ((__unused__)) *data)
{
	return -1;
}

/*
 * Without LIBCONFIG, let run with current user
 */
static inline int read_settings_user_id(const char __attribute__ ((__unused__))*filename,
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
