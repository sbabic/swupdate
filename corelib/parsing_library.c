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

void check_field_string(const char *src, char *dst, const size_t max_len)
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
