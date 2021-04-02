/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _PARSE_LIBRARY_H
#define _PARSE_LIBRARY_H

#include <assert.h>
#include <stdbool.h>

typedef enum {
	LIBCFG_PARSER,
	JSON_PARSER
} parsertype;

typedef void (*iterate_callback)(const char *name, const char *value,
				 void *data);

/*
 * This is to limit the structure (array) used to save the whole
 * path to the entry to be read.
 */
#define MAX_PARSED_NODES	20

#ifdef CONFIG_LIBCONFIG
#include <libconfig.h>
#define LIBCONFIG_VERSION ((LIBCONFIG_VER_MAJOR << 16) | \
	(LIBCONFIG_VER_MINOR << 8) | LIBCONFIG_VER_REVISION)
#if LIBCONFIG_VERSION < 0x10500
#define config_setting_lookup config_lookup_from
#endif

void get_value_libconfig(const config_setting_t *e, void *dest);
void get_field_cfg(config_setting_t *e, const char *path, void *dest);
void *get_child_libconfig(void *e, const char *name);
void iterate_field_libconfig(config_setting_t *e, iterate_callback cb,
			     void *data);
const char *get_field_string_libconfig(config_setting_t *e, const char *path);
void *find_root_libconfig(config_t *cfg, const char **nodes, unsigned int depth);
void *get_node_libconfig(config_t *cfg, const char **nodes);

#else
#define config_setting_get_elem(a,b)	(NULL)
#define config_setting_length(a)	(0)
#define config_setting_lookup_string(a, b, str) (0)
#define find_node_libconfig(cfg, field, swcfg) (NULL)
#define get_field_string_libconfig(e, path)	(NULL)
#define get_child_libconfig(e, name)		(NULL)
#define iterate_field_libconfig(e, cb, data)	{ }
#define get_field_cfg(e, path, dest)
#define find_root_libconfig(cfg, nodes, depth)		(NULL)
#define get_node_libconfig(cfg, nodes)		(NULL)
#endif

#ifdef CONFIG_JSON
#include <json-c/json.h>

const char *get_field_string_json(json_object *e, const char *path);
void get_value_json(json_object *e, void *dest);
void get_field_json(json_object *e, const char *path, void *dest);
void *get_child_json(json_object *e, const char *name);
void iterate_field_json(json_object *e, iterate_callback cb, void *data);
json_object *find_json_recursive_node(json_object *root, const char **names);
json_object *json_get_key(json_object *json_root, const char *key);
const char *json_get_value(struct json_object *json_root,
			   const char *key);
json_object *json_get_path_key(json_object *json_root, const char **json_path);
char *json_get_data_url(json_object *json_root, const char *key);
void *find_root_json(json_object *root, const char **nodes, unsigned int depth);
void *get_node_json(json_object *root, const char **nodes);

#else
#define find_node_json(a, b, c)		(NULL)
#define get_field_string_json(e, path)  (NULL)
#define get_child_json(e, name)		(NULL)
#define iterate_field_json(e, cb, data)	{ }
#define get_field_json(e, path, dest)
#define json_object_object_get_ex(a,b,c) (0)
#define json_object_array_get_idx(a, b)	(0)
#define json_object_array_length(a)	(0)
#define find_root_json(root, nodes, depth)	(NULL)
#define get_node_json(root, nodes)	(NULL)
#endif

const char *get_field_string(parsertype p, void *e, const char *path);
void get_field_string_with_size(parsertype p, void *e, const char *path,
				char *d, size_t n);
int get_array_length(parsertype p, void *root);
void *get_elem_from_idx(parsertype p, void *node, int idx);
void *get_child(parsertype p, void *node, const char *name);
void iterate_field(parsertype p, void *e, iterate_callback cb, void *data);
void get_field(parsertype p, void *e, const char *path, void *dest);
int exist_field_string(parsertype p, void *e, const char *path);
void get_hash_value(parsertype p, void *elem, unsigned char *hash);
void check_field_string(const char *src, char *dst, const size_t max_len);
void *find_root(parsertype p, void *root, const char **nodes);
void *get_node(parsertype p, void *root, const char **nodes);
bool set_find_path(const char **nodes, const char *newpath, char **tmp);

#define GET_FIELD_STRING(p, e, name, d) \
	get_field_string_with_size(p, e, name, d, sizeof(d))

#define GET_FIELD_STRING_RESET(p, e, name, d) do { \
	d[0] = '\0'; \
	GET_FIELD_STRING(p, e, name, d); \
} while (0)


#endif
