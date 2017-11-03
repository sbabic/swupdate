/* (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <assert.h>
#include "generated/autoconf.h"
#include "bsdqueue.h"
#include "util.h"
#include "swupdate.h"
#include "parselib.h"

void get_value_libconfig(const config_setting_t *e, void *dest)
{
	int type = config_setting_type(e);
	switch (type) {
	case CONFIG_TYPE_INT:
		*(int *)dest = config_setting_get_int(e);
		break;
	case CONFIG_TYPE_INT64:
		*(long long *)dest = config_setting_get_int64(e);
		break;
	case CONFIG_TYPE_STRING:
		dest = (void *)config_setting_get_string(e);
		break;
	case CONFIG_TYPE_BOOL:
		*(int *)dest = config_setting_get_bool(e);
		break;
	case CONFIG_TYPE_FLOAT:
		*(double *)dest = config_setting_get_float(e);
		break;
		/* Do nothing, add if needed */
	}
}

void *get_child_libconfig(void *e, const char *name)
{
	return config_setting_get_member(e, name);
}

void get_field_cfg(config_setting_t *e, const char *path, void *dest)
{
	config_setting_t *elem;

	if (path)
		elem = config_setting_lookup(e, path);
	else
		elem = e;

	if (!elem)
		return;

	get_value_libconfig(elem, dest);
}

const char *get_field_string_libconfig(config_setting_t *e, const char *path)
{
	config_setting_t *elem;
	const char *str;

	if (path)
		elem = config_setting_lookup(e, path);
	else
		elem = e;

	if (!elem || config_setting_type(elem) != CONFIG_TYPE_STRING)
		return NULL;

	if ( ( ( path) && (config_setting_lookup_string(e, path, &str))  ) ||
	     ( (!path) && ((str = config_setting_get_string(e)) != NULL) ) ) {

		return str;

	}

	return NULL;
}
