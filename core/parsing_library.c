/* (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
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

void *find_root(parsertype p, void *root, const char **nodes)
{

	switch (p) {
	case LIBCFG_PARSER:
		return find_root_libconfig((config_t *)root, nodes, MAX_LINKS_DEPTH);

	case JSON_PARSER:
		return find_root_json((json_object *)root, nodes, MAX_LINKS_DEPTH);
	default:
		(void)root;
		(void)nodes;
	}

	return NULL;
}

void *get_node(parsertype p, void *root, const char **nodes)
{

	switch (p) {
	case LIBCFG_PARSER:
		return get_node_libconfig((config_t *)root, nodes);
	case JSON_PARSER:
		return get_node_json((json_object *)root, nodes);
	default:
		(void)root;
		(void)nodes;
	}

	return NULL;
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
	char **paths;
	unsigned int count;
	char *saveptr;
	char *token, *ref;
	bool first = true;
	int allocstr = 0;
	(void)tmp;

	/*
	 * Include of files is not supported,
	 * each reference should start with '#'
	 */
	if (!newpath || (newpath[0] != '#') || (strlen(newpath) < 3))
		return false;

	ref = strdup(newpath);
	if (!ref) {
		ERROR("No memory: failed for %zu bytes",
		       strlen(newpath) + 1);
		return false;
	}

	/*
	 * First find how many strings should be stored
	 * Each token is stored temporarily on the heap
	 */
	count = 0;
	token = strtok_r(&ref[1], "/", &saveptr);
	while (token) {
		count++;
		token = strtok_r(NULL, "/", &saveptr);
	}
	free(ref); /* string was changed, clean and copy again */

	if (!count)
		return false;

	paths = calloc(count + 1, sizeof(char*) * count);
	if (!paths) {
		ERROR("No memory: calloc failed for %zu bytes",
		       sizeof(char*) * count);
		return false;
	}
	count = count_string_array(nodes);
	/*
	 * Count is surely > 0, decrementing is safe
	 * Do not consider the last leaf with "ref"
	 * This means that "#./link" is searched
	 * starting from the parent of "ref"
	 */
	count--;

	ref = strdup(newpath);
	if (!ref) {
		ERROR("No memory: failed for %zu bytes",
		       strlen(newpath) + 1);
		free(paths);
		return false;
	}
	
	token = strtok_r(&ref[1], "/", &saveptr);
	while (token) {
		if (!strcmp(token, "..")) {
			if (!count) {
				free(ref);
				free_string_array(paths);
				return false;
			}
			count--;
		} else if (strcmp(token, ".")) {
			if (first) {
				count = 0;
			}
			paths[allocstr] = strdup(token);
			nodes[count++] = paths[allocstr++];
			paths[allocstr] = NULL;
			nodes[count] = NULL;
			if (count >= MAX_PARSED_NODES) {
				ERROR("Big depth in link, giving up..."); 
				free_string_array(paths);
				free(ref);
				return false;
			}
		}
		token = strtok_r(NULL, "/", &saveptr);
		first = false;
	}

	free(ref);
	tmp = paths;

	return true;
}
