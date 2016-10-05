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

static void check_field_string(const char *src, char *dst, const size_t max_len)
{
	assert(max_len>0);
	size_t act_len = strnlen(src, SWUPDATE_GENERAL_STRING_SIZE);
	if (act_len > max_len) {
		((char*)dst)[max_len-1] = '\0';
		WARN("Configuration Key '%s...' is > %u chars, cropping it.\n",
			(char*)dst, (unsigned int)max_len - 1);
	}
	if (act_len == 0) {
		WARN("Configuration Key is empty!\n");
	}
}

#ifdef CONFIG_LIBCONFIG
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

void get_field_string_libconfig(config_setting_t *e, const char *path, void *dest, size_t n)
{
	config_setting_t *elem;
	const char *str;

	if (path)
		elem = config_setting_lookup(e, path);
	else
		elem = e;

	if (!elem || config_setting_type(elem) != CONFIG_TYPE_STRING)
		return;

	if ( ( ( path) && (config_setting_lookup_string(e, path, &str))  ) ||
	     ( (!path) && ((str = config_setting_get_string(e)) != NULL) ) ) {

		if (!n) {
			n = strlen(str);
			dest = malloc(strlen(str) + 1);
			if (!dest)
				return;
		}

		strncpy(dest, str, n);
		check_field_string(str, dest, n);
	}
}
#endif

#ifdef CONFIG_JSON

json_object *find_json_recursive_node(json_object *root, const char **names)
{
	json_object *node = root;

	while (*names) {
		const char *n = *names;
		json_object *cnode = NULL;

		if (json_object_object_get_ex(node, n, &cnode))
			node = cnode;
		else
			return NULL;
		names++;
	}

	return node;
}

void get_field_string_json(json_object *e, const char *path, char *dest, size_t n)
{
	const char *str;
	json_object *node;

	if (json_object_object_get_ex(e, path, &node) &&
		(json_object_get_type(node) == json_type_string)) {
		str = json_object_get_string(node);

		if (!n) {
			n = strlen(str);
			dest = malloc(strlen(str) + 1);
			if (!dest)
				return;
		}

		strncpy(dest, str, n);
		check_field_string(str, dest, n);
	}
}

void get_value_json(json_object *e, void *dest)
{
	enum json_type type;
	type = json_object_get_type(e);
	switch (type) {
	case json_type_boolean:
		*(unsigned int *)dest = json_object_get_boolean(e);
		break;
	case json_type_int:
		*(unsigned int *)dest = json_object_get_int(e);
		break;
	case json_type_string:
		strcpy(dest, json_object_get_string(e));
		break;
	case json_type_double:
		*(double *)dest = json_object_get_double(e);
		break;
	default:
		break;
	}
}

void get_field_json(json_object *e, const char *path, void *dest)
{
	json_object *fld = NULL;

	if (path) {
		if (json_object_object_get_ex(e, path, &fld))
			get_value_json(fld, dest);
	} else {
		get_value_json(e, dest);
	}
}
#endif

int get_array_length(parsertype p, void *root)
{
	switch (p) {
	case LIBCFG_PARSER:
		return config_setting_length(root);
	case JSON_PARSER:
		return json_object_array_length(root);
	}

	return 0;
}

void *get_elem_from_idx(parsertype p, void *node, int idx)
{
	switch (p) {
	case LIBCFG_PARSER:
		return config_setting_get_elem(node, idx);
	case JSON_PARSER:
		return json_object_array_get_idx(node, idx);
	}

	return NULL;
}

void get_field_string(parsertype p, void *e, const char *path, char *dest, size_t n)
{
	switch (p) {
	case LIBCFG_PARSER:
		get_field_string_libconfig(e, path, dest, n);
		break;
	case JSON_PARSER:
		get_field_string_json(e, path, dest, n);
		break;
	}
}

void get_field(parsertype p, void *e, const char *path, void *dest)
{
	switch (p) {
	case LIBCFG_PARSER:
		return get_field_cfg((config_setting_t *)e, path, dest);
	case JSON_PARSER:
		return get_field_json((json_object *)e, path, dest);
	}
}

int exist_field_string(parsertype p, void *e, const char *path)
{
	const char *str;
	switch (p) {
	case LIBCFG_PARSER:
		return config_setting_lookup_string((const config_setting_t *)e,
							path, &str);
	case JSON_PARSER:
		return json_object_object_get_ex((json_object *)e,  path, NULL);
	}

	return 0;
}

void get_hash_value(parsertype p, void *elem, unsigned char *hash)
{
	char hash_ascii[80];

	memset(hash_ascii, 0, sizeof(hash_ascii));
	GET_FIELD_STRING(p, elem, "sha256", hash_ascii);

	ascii_to_hash(hash, hash_ascii);
}
