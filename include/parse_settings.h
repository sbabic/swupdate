/*
 * (C) Copyright 2013
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

#ifndef _PARSE_SETTINGS_H
#define _PARSE_SETTINGS_H

#define GET_FIELD_STRING(p, e, name, d) \
	get_field_string(p, e, name, d, sizeof(d))

#ifdef CONFIG_LIBCONFIG
#include <libconfig.h>
#define LIBCONFIG_VERSION ((LIBCONFIG_VER_MAJOR << 16) | \
	(LIBCONFIG_VER_MINOR << 8) | LIBCONFIG_VER_REVISION)
#if LIBCONFIG_VERSION < 0x10500
#define config_setting_lookup config_lookup_from
#endif

void get_value_libconfig(const config_setting_t *e, void *dest);
void get_field_cfg(config_setting_t *e, const char *path, void *dest);
void get_field_string_libconfig(config_setting_t *e, const char *path,
	       			void *dest, size_t n);

#else
#define config_setting_get_elem(a,b)	(NULL)
#define config_setting_length(a)	(0)
#define config_setting_lookup_string(a, b, str) (0)
#define find_node_libconfig(cfg, field, swcfg) (NULL)
#define get_field_string_libconfig(e, path, dest, n)
#define get_field_cfg(e, path, dest)
#endif

#ifdef CONFIG_JSON
#include <json-c/json.h>

void get_field_string_json(json_object *e, const char *path, char *dest, size_t n);
void get_value_json(json_object *e, void *dest);
void get_field_json(json_object *e, const char *path, void *dest);

#else
#define find_node_json(a, b, c)		(NULL)
#define get_field_string_json(e, path, dest, n)
#define get_field_json(e, path, dest)
#define json_object_object_get_ex(a,b,c) (0)
#define json_object_array_get_idx(a, b)	(0)
#define json_object_array_length(a)	(0)
#endif

#endif


