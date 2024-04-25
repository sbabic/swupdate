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

#ifdef CONFIG_LIBCONFIG
bool is_field_numeric_cfg(config_setting_t *e, const char *path);
void get_field_cfg(config_setting_t *e, const char *path, void *dest, field_type_t type);
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
#define get_field_cfg(e, path, dest, type)
#define find_root_libconfig(cfg, nodes, depth)		(NULL)
#define get_node_libconfig(cfg, nodes)		(NULL)
#define is_field_numeric_cfg(e, path)	(false)
#endif

/*
 * JSON implementation for parselib
 */
bool is_field_numeric_json(json_object *e, const char *path);
const char *get_field_string_json(json_object *e, const char *path);
void get_field_json(json_object *e, const char *path, void *dest, field_type_t type);
void *get_child_json(json_object *e, const char *name);

void iterate_field_json(json_object *e, iterate_callback cb, void *data);
json_object *find_json_recursive_node(json_object *root, const char **names);
void *find_root_json(json_object *root, const char **nodes, unsigned int depth);
void *get_node_json(json_object *root, const char **nodes);
