/*
 * (C) Copyright 2016
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

#ifndef _PARSE_LIBRARY_H
#define _PARSE_LIBRARY_H

#include <assert.h>

typedef enum {
	LIBCFG_PARSER,
	JSON_PARSER
} parsertype;

#ifdef CONFIG_LIBCONFIG
#include <libconfig.h>
#define LIBCONFIG_VERSION ((LIBCONFIG_VER_MAJOR << 16) | \
	(LIBCONFIG_VER_MINOR << 8) | LIBCONFIG_VER_REVISION)
#if LIBCONFIG_VERSION < 0x10500
#define config_setting_lookup config_lookup_from
#endif

void get_value_libconfig(const config_setting_t *e, void *dest);
void get_field_cfg(config_setting_t *e, const char *path, void *dest);
const char *get_field_string_libconfig(config_setting_t *e, const char *path);

#else
#define config_setting_get_elem(a,b)	(NULL)
#define config_setting_length(a)	(0)
#define config_setting_lookup_string(a, b, str) (0)
#define find_node_libconfig(cfg, field, swcfg) (NULL)
#define get_field_string_libconfig(e, path)	(NULL)
#define get_field_cfg(e, path, dest)
#endif

#ifdef CONFIG_JSON
#include <json-c/json.h>

const char *get_field_string_json(json_object *e, const char *path);
void get_value_json(json_object *e, void *dest);
void get_field_json(json_object *e, const char *path, void *dest);
json_object *find_json_recursive_node(json_object *root, const char **names);

#else
#define find_node_json(a, b, c)		(NULL)
#define get_field_string_json(e, path, dest, n)
#define get_field_json(e, path, dest)
#define json_object_object_get_ex(a,b,c) (0)
#define json_object_array_get_idx(a, b)	(0)
#define json_object_array_length(a)	(0)
#endif

typedef int (*settings_callback)(void *elem, void *data);

const char *get_field_string(parsertype p, void *e, const char *path);
void get_field_string_with_size(parsertype p, void *e, const char *path,
				char *d, size_t n);
int get_array_length(parsertype p, void *root);
void *get_elem_from_idx(parsertype p, void *node, int idx);
void get_field(parsertype p, void *e, const char *path, void *dest);
int exist_field_string(parsertype p, void *e, const char *path);
void get_hash_value(parsertype p, void *elem, unsigned char *hash);
void check_field_string(const char *src, char *dst, const size_t max_len);

#define GET_FIELD_STRING(p, e, name, d) \
	get_field_string_with_size(p, e, name, d, sizeof(d))

#define GET_FIELD_STRING_RESET(p, e, name, d) do { \
	d[0] = '\0'; \
	GET_FIELD_STRING(p, e, name, d); \
} while (0)


#endif
