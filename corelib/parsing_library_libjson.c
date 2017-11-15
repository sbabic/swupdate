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

#define MAX_URL_LENGTH 2048

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

void *get_child_json(json_object *e, const char *path)
{
	json_object *node = NULL;

	if (path) {
		if (!json_object_object_get_ex(e, path, &node))
			return NULL;
	}

	return node;
}

const char *get_field_string_json(json_object *e, const char *path)
{
	const char *str;
	json_object *node;

	if (path) {
		if (!json_object_object_get_ex(e, path, &node))
			return NULL;
	} else
		node = e;

	if (json_object_get_type(node) == json_type_string) {
		str = json_object_get_string(node);

		return str;
	}

	return NULL;
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

json_object *json_get_key(json_object *json_root, const char *key)
{
	json_object *json_child;
	if (json_object_object_get_ex(json_root, key, &json_child)) {
		return json_child;
	}
	return NULL;
}

const char *json_get_value(struct json_object *json_root,
			   const char *key)
{
	json_object *json_data = json_get_key(json_root, key);

	if (json_data == NULL)
		return "";

	return json_object_get_string(json_data);
}

json_object *json_get_path_key(json_object *json_root, const char **json_path)
{
	json_object *json_data = json_root;
	while (*json_path) {
		const char *key = *json_path;
		json_data = json_get_key(json_data, key);
		if (json_data == NULL) {
			return NULL;
		}
		json_path++;
	}
	return json_data;
}

char *json_get_data_url(json_object *json_root, const char *key)
{
	json_object *json_data = json_get_path_key(
	    json_root, (const char *[]){"_links", key, "href", NULL});
	return json_data == NULL
		   ? NULL
		   : strndup(json_object_get_string(json_data), MAX_URL_LENGTH);
}


