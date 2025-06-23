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

bool is_field_numeric_cfg(config_setting_t *e, const char *path);
void get_field_cfg(config_setting_t *e, const char *path, void *dest, field_type_t type);
void *get_child_libconfig(void *e, const char *name);
void iterate_field_libconfig(config_setting_t *e, iterate_callback cb,
			     void *data);
const char *get_field_string_libconfig(config_setting_t *e, const char *path);
void *find_root_libconfig(config_t *cfg, const char **nodes, unsigned int depth);
void *get_node_libconfig(config_t *cfg, const char **nodes);

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
