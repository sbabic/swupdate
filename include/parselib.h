/*
 * (C) Copyright 2016-2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <assert.h>
#include <stdbool.h>
#include <json-c/json.h>
#include <libconfig.h>
#define LIBCONFIG_VERSION ((LIBCONFIG_VER_MAJOR << 16) | \
	(LIBCONFIG_VER_MINOR << 8) | LIBCONFIG_VER_REVISION)
#if LIBCONFIG_VERSION < 0x10500
#define config_setting_lookup config_lookup_from
#endif

typedef enum {
	LIBCFG_PARSER,
	JSON_PARSER
} parsertype;

typedef enum {
	TYPE_INT,
	TYPE_INT64,
	TYPE_BOOL,
	TYPE_DOUBLE
} field_type_t;

typedef void (*iterate_callback)(const char *name, const char *value,
				 void *data);

/*
 * This is to limit the structure (array) used to save the whole
 * path to the entry to be read.
 */
#define MAX_PARSED_NODES	20

const char *json_get_value(struct json_object *json_root,
			   const char *key);
json_object *json_get_key(json_object *json_root, const char *key);
json_object *json_get_path_key(json_object *json_root, const char **json_path);
char *json_get_data_url(json_object *json_root, const char *key);

/*
 * Parselib interface
 */
bool is_field_numeric(parsertype p, void *e, const char *path);
bool is_field_bool(parsertype p, void *e, const char *path);
bool is_field_string(parsertype p, void *e, const char *path);
const char *get_field_string(parsertype p, void *e, const char *path);
void get_field_string_with_size(parsertype p, void *e, const char *path,
				char *d, size_t n);
int get_array_length(parsertype p, void *root);
void *get_elem_from_idx(parsertype p, void *node, int idx);
void *get_child(parsertype p, void *node, const char *name);
void iterate_field(parsertype p, void *e, iterate_callback cb, void *data);
void get_field(parsertype p, void *e, const char *path, void *dest, field_type_t type);
int exist_field_string(parsertype p, void *e, const char *path);
void get_hash_value(parsertype p, void *elem, unsigned char *hash);
void check_field_string(const char *src, char *dst, const size_t max_len);
void *find_root(parsertype p, void *root, const char **nodes);
void *get_node(parsertype p, void *root, const char **nodes);
bool set_find_path(const char **nodes, const char *newpath);

static inline void get_field_bool(parsertype p, void *e, const char *path, bool *dest)
{
	get_field(p, e, path, dest, TYPE_BOOL);
}

static inline void get_field_int(parsertype p, void *e, const char *path, int *dest)
{
	get_field(p, e, path, dest, TYPE_INT);
}

static inline void get_field_int64(parsertype p, void *e, const char *path, long long *dest)
{
	get_field(p, e, path, dest, TYPE_INT64);
}

static inline void get_field_float(parsertype p, void *e, const char *path, double *dest)
{
	get_field(p, e, path, dest, TYPE_DOUBLE);
}

#define GET_FIELD_STRING(p, e, name, d) \
	get_field_string_with_size(p, e, name, d, sizeof(d))

#define GET_FIELD_STRING_RESET(p, e, name, d) do { \
	d[0] = '\0'; \
	GET_FIELD_STRING(p, e, name, d); \
} while (0)

#define GET_FIELD_BOOL(p, e, path, dest) \
	get_field_bool(p, e, path, dest)

#define GET_FIELD_INT(p, e, path, dest) \
	get_field_int(p, e, path, dest)

#define GET_FIELD_INT64(p, e, path, dest) \
	get_field_int64(p, e, path, dest)

#define GET_FIELD_FLOAT(p, e, path, dest) \
	get_field_float(p, e, path, dest)
