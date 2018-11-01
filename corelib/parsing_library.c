/* (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <assert.h>
#include "generated/autoconf.h"
#include "bsdqueue.h"
#include "util.h"
#include "swupdate.h"
#include "parselib.h"

#define MAX_LINKS_DEPTH	10

void check_field_string(const char *src, char *dst, const size_t max_len)
{
	assert(max_len>0);
	size_t act_len = strnlen(src, SWUPDATE_GENERAL_STRING_SIZE);
	if (act_len > max_len) {
		((char*)dst)[max_len-1] = '\0';
		WARN("Configuration Key '%s...' is > %u chars, cropping it.",
			(char*)dst, (unsigned int)max_len - 1);
	}
	if (act_len == 0) {
		WARN("Configuration Key is empty!");
	}
}

int get_array_length(parsertype p, void *root)
{
	switch (p) {
	case LIBCFG_PARSER:
		return config_setting_length(root);
	case JSON_PARSER:
		return json_object_array_length(root);
	default:
		(void)root;
	}

	return 0;
}

void *get_child(parsertype p, void *e, const char *name)
{
	switch (p) {
	case LIBCFG_PARSER:
		return get_child_libconfig(e, name);
	case JSON_PARSER:
		return get_child_json((json_object *)e, name);
	default:
		(void)e;
		(void)name;
	}

	return NULL;
}

void iterate_field(parsertype p, void *e, iterate_callback cb, void *data)
{
	switch (p) {
	case LIBCFG_PARSER:
		iterate_field_libconfig(e, cb, data);
		break;
	case JSON_PARSER:
		iterate_field_json(e, cb, data);
		break;
	default:
		(void)e;
		(void)cb;
		(void)data;
	}
}

void *get_elem_from_idx(parsertype p, void *node, int idx)
{
	switch (p) {
	case LIBCFG_PARSER:
		return config_setting_get_elem(node, idx);
	case JSON_PARSER:
		return json_object_array_get_idx(node, idx);
	default:
		(void)node;
		(void)idx;
	}

	return NULL;
}

const char *get_field_string(parsertype p, void *e, const char *path)
{
	switch (p) {
	case LIBCFG_PARSER:
		return get_field_string_libconfig(e, path);
	case JSON_PARSER:
		return get_field_string_json(e, path);
	default:
		(void)e;
		(void)path;
	}

	return NULL;
}

void get_field_string_with_size(parsertype p, void *e, const char *path, char *d, size_t n)
{
	const char *s = NULL;
	s = get_field_string(p, e, path);
	if (s) {
		strncpy(d, s, n);
		check_field_string(s, d, n);
	}
}

void get_field(parsertype p, void *e, const char *path, void *dest)
{
	switch (p) {
	case LIBCFG_PARSER:
		return get_field_cfg((config_setting_t *)e, path, dest);
	case JSON_PARSER:
		return get_field_json((json_object *)e, path, dest);
	default:
		(void)e;
		(void)path;
		(void)dest;
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
	default:
		(void)str;
		(void)e;
		(void)path;
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

bool set_find_path(const char **nodes, const char *newpath, char **tmp)
{
	unsigned int nleading;
	char **iter, **paths;
	unsigned int count = count_string_array(nodes);
	unsigned int countpaths;

	if (!newpath)
		return false;

	/*
	 * Check if we have to traverse back
	 */
	for (nleading = 0; newpath[nleading] == '.'; nleading++);

	/*
	 * delimiter at the beginning indicates a relative path
	 * exactly as in Unix, that mean .. for the upper directory
	 * .. = parent 
	 * .... = parent of parent
	 * The number of leading "." must be even, else
	 * it is a malformed path
	 */
	if (nleading % 2)
		return false;

	nleading /= 2;
	if ((count - nleading) <= 0)
		return false;

	count -= nleading;
	if (count > 0) count--;

	paths = string_split(newpath, '.');

	/*
	 * check if there is enough space in nodes
	 */
	countpaths = count_string_array((const char **)paths);
	if (count + countpaths >= MAX_PARSED_NODES)
		return false;
	if (!countpaths)
		nodes[count++] = newpath;
	else
		for (iter = paths; *iter != NULL; iter++, count++)
			nodes[count] = *iter;
	nodes[count] = NULL;

	tmp = paths;

	return true;
}
