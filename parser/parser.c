/*
 * (C) Copyright 2015
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
#include "generated/autoconf.h"
#include "bsdqueue.h"
#include "util.h"
#include "swupdate.h"
#include "parsers.h"

#ifdef CONFIG_LIBCONFIG
#include <libconfig.h>
#define LIBCONFIG_VERSION ((LIBCONFIG_VER_MAJOR << 16) | \
	(LIBCONFIG_VER_MINOR << 8) | LIBCONFIG_VER_REVISION)
#if LIBCONFIG_VERSION < 0x10500
#define config_setting_lookup config_lookup_from
#endif
#else
#define config_setting_get_elem(a,b)	(NULL)
#define config_setting_length(a)	(0)
#define config_setting_lookup_string(a, b, str) (0)
#define find_node_libconfig(cfg, field, swcfg) (NULL)
#define get_field_string_libconfig(e, path, dest, n)
#define get_field_cfg(e, path, dest)
#endif

#ifdef CONFIG_JSON
#include <json-c/json.h>
#else
#define find_node_json(a, b, c)		(NULL)
#define get_field_string_json(e, path, dest, n)
#define get_field_json(e, path, dest)
#define json_object_object_get_ex(a,b,c) (0)
#define json_object_array_get_idx(a, b)	(0)
#define json_object_array_length(a)	(0)
#endif

#define MODULE_NAME	"PARSER"

#define GET_FIELD_STRING(p, e, name, d) \
	get_field_string(p, e, name, d, sizeof(d))

#define NODEROOT (!strlen(CONFIG_LIBCONFIGROOT) ? \
			"software" : CONFIG_LIBCONFIGROOT)

typedef enum {
	LIBCFG_PARSER,
	JSON_PARSER
} parsertype;

static struct hw_type hardware;

#ifdef CONFIG_LIBCONFIG
static config_setting_t *find_node_libconfig(config_t *cfg,
					const char *field, struct swupdate_cfg *swcfg)
{
	//const config_setting_t *setting;
	config_setting_t *setting;

	char node[1024];

	if (!field)
		return NULL;

	if (strlen(swcfg->running_mode) && strlen(swcfg->software_set)) {
		/* Try with both software set and board name */
		if (strlen(hardware.boardname)) {
			snprintf(node, sizeof(node), "%s.%s.%s.%s.%s",
				NODEROOT,
				hardware.boardname,
				swcfg->software_set,
				swcfg->running_mode,
				field);
			setting = config_lookup(cfg, node);
			if (setting)
				return setting;
		}
		/* still here, try with software set and mode */
		snprintf(node, sizeof(node), "%s.%s.%s.%s",
			NODEROOT,
			swcfg->software_set,
			swcfg->running_mode,
			field);
		setting = config_lookup(cfg, node);
		if (setting)
			return setting;

	}

	/* Try with board name */
	if (strlen(hardware.boardname)) {
		snprintf(node, sizeof(node), "%s.%s.%s",
			NODEROOT,
			hardware.boardname,
			field);
		setting = config_lookup(cfg, node);
		if (setting)
			return setting;
	}
	/* Fall back without board entry */
	snprintf(node, sizeof(node), "%s.%s",
		NODEROOT,
		field);
	return config_lookup(cfg, node);
}

static void get_value_libconfig(const config_setting_t *e, void *dest)
{
	int type = config_setting_type(e);
	switch (type) {
	case CONFIG_TYPE_INT:
		*(int *)dest = config_setting_get_int(e);
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
		/* Do nothing, add if needed */
	}
}

static void get_field_cfg(config_setting_t *e, const char *path, void *dest)
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

static void get_field_string_libconfig(config_setting_t *e, const char *path, void *dest, size_t n)
{
	config_setting_t *elem;
	const char *str;

	if (path)
		elem = config_setting_lookup(e, path);
	else
		elem = e;

	if (!elem || config_setting_type(elem) != CONFIG_TYPE_STRING)
		return;

	if (path) {
		if (config_setting_lookup_string(e, path, &str))
			strncpy(dest, str, n);
	} else {
		if ((str = config_setting_get_string(e)) != NULL)
			strncpy(dest, str, n);
	}
}
#endif

#ifdef CONFIG_JSON
static json_object *find_recursive_node(json_object *root, const char **names)
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

static json_object *find_node_json(json_object *root, const char *node,
			struct swupdate_cfg *swcfg)
{
	json_object *jnode = NULL;
	const char *simple_nodes[] = {node, NULL};

	if (strlen(swcfg->running_mode) && strlen(swcfg->software_set)) {
		if (strlen(hardware.boardname)) {
			const char *nodes[] = {hardware.boardname, swcfg->software_set,
					       swcfg->running_mode, node, NULL};
			jnode = find_recursive_node(root, nodes);
			if (jnode)
				return jnode;
		} else {
			const char *nodes[] = {swcfg->software_set, swcfg->running_mode,
					       node, NULL};
			jnode = find_recursive_node(root, nodes);
			if (jnode)
				return jnode;
		}
	}

	if (strlen(hardware.boardname)) {
		const char *nodes[] = {hardware.boardname, node, NULL};
		jnode = find_recursive_node(root, nodes);
		if (jnode)
			return jnode;
	}

	return find_recursive_node(root, simple_nodes);
}

static void get_field_string_json(json_object *e, const char *path, char *dest, size_t n)
{
	const char *str;
	json_object *node;

	if (json_object_object_get_ex(e, path, &node) &&
		(json_object_get_type(node) == json_type_string)) {
		str = json_object_get_string(node);
		strncpy(dest, str, n);
	}
}

static void get_value_json(json_object *e, void *dest)
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

static void get_field_json(json_object *e, const char *path, void *dest)
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

static int get_array_length(parsertype p, void *root)
{
	switch (p) {
	case LIBCFG_PARSER:
		return config_setting_length(root);
	case JSON_PARSER:
		return json_object_array_length(root);
	}

	return 0;
}

static void *get_elem_from_idx(parsertype p, void *node, int idx)
{
	switch (p) {
	case LIBCFG_PARSER:
		return config_setting_get_elem(node, idx);
	case JSON_PARSER:
		return json_object_array_get_idx(node, idx);
	}

	return NULL;
}


static void *find_node(parsertype p, void *root, const char *node,
			struct swupdate_cfg *swcfg)
{
	switch (p) {
	case LIBCFG_PARSER:
		return find_node_libconfig((config_t *)root, node, swcfg);
	case JSON_PARSER:
		return find_node_json((json_object *)root, node, swcfg);
	}

	return NULL;
}

static void get_field_string(parsertype p, void *e, const char *path, char *dest, size_t n)
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

static void get_field(parsertype p, void *e, const char *path, void *dest)
{
	switch (p) {
	case LIBCFG_PARSER:
		return get_field_cfg((config_setting_t *)e, path, dest);
	case JSON_PARSER:
		return get_field_json((json_object *)e, path, dest);
	}
}

static int exist_field_string(parsertype p, void *e, const char *path)
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

#ifdef CONFIG_HW_COMPATIBILITY
/*
 * Check if the software can run on the hardware
 */
static int parse_hw_compatibility(parsertype p, void *cfg, struct swupdate_cfg *swcfg)
{
	void *setting, *hw;
	int count, i;
	char s[SWUPDATE_GENERAL_STRING_SIZE];
	struct hw_type *hwrev;

	setting = find_node(p, cfg, "hardware-compatibility", swcfg);
	if (setting == NULL) {
		ERROR("HW compatibility not found\n");
		return -1;
	}

	count = get_array_length(p, setting);

	for(i = 0; i < count; ++i) {
		hw = get_elem_from_idx(p, setting, i);

		if (!hw)
			continue;

		s[0] = '\0';
		GET_FIELD_STRING(p, hw, NULL, s);
		if (!strlen(s))
			continue;

		hwrev = (struct hw_type *)calloc(1, sizeof(struct hw_type));
		if (!hwrev) {
			ERROR("No memory: malloc failed\n");
			return -1;
		}

		strncpy(hwrev->revision, s, sizeof(hwrev->revision));
		LIST_INSERT_HEAD(&swcfg->hardware, hwrev, next);
		TRACE("Accepted Hw Revision : %s", hwrev->revision);
	}

	return 0;
}
#else
static int parse_hw_compatibility(parsertype __attribute__ ((__unused__))p,
		void __attribute__ ((__unused__))  *cfg,
		struct swupdate_cfg __attribute__ ((__unused__)) *swcfg)
{
	return 0;
}
#endif

static void parse_partitions(parsertype p, void *cfg, struct swupdate_cfg *swcfg)
{
	void *setting, *elem;
	int count, i;
	struct img_type *partition;

	setting = find_node(p, cfg, "partitions", swcfg);

	if (setting == NULL)
		return;

	count = get_array_length(p, setting);
	for(i = 0; i < count; ++i) {
		elem = get_elem_from_idx(p, setting, i);

		if (!elem)
			continue;

		partition = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!partition) {
			ERROR("No memory: malloc failed\n");
			return;
		}
		GET_FIELD_STRING(p, elem, "name", partition->volname);
		GET_FIELD_STRING(p, elem, "device", partition->device);
		strncpy(partition->type, "ubipartition", sizeof(partition->type));
		partition->is_partitioner = 1;

		partition->provided = 1;

		if (!partition->volname || !partition->device) {
			ERROR("Partition incompleted in description file");
			return;
		}

		get_field(p, elem, "size", &partition->partsize);

		TRACE("Partition: %s new size %lld bytes\n",
			partition->volname,
			partition->partsize);

		LIST_INSERT_HEAD(&swcfg->images, partition, next);
	}
}

static void parse_scripts(parsertype p, void *cfg, struct swupdate_cfg *swcfg)
{
	void *setting, *elem;
	int count, i;
	struct img_type *script;

	setting = find_node(p, cfg, "scripts", swcfg);

	if (setting == NULL)
		return;

	count = get_array_length(p, setting);
	/*
	 * Scan the scripts in reserve order to maintain
	 * the same order using LIST_INSERT_HEAD
	 */
	for(i = (count - 1); i >= 0; --i) {
		elem = get_elem_from_idx(p, setting, i);

		if (!elem)
			continue;

		if(!(exist_field_string(p, elem, "filename")))
			continue;

		script = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!script) {
			ERROR( "No memory: malloc failed\n");
			return;
		}

		GET_FIELD_STRING(p, elem, "filename", script->fname);
		GET_FIELD_STRING(p, elem, "type", script->type);

		/* Scripts as default call the LUA interpreter */
		if (!strlen(script->type)) {
			strcpy(script->type, "lua");
		}
		script->is_script = 1;

		LIST_INSERT_HEAD(&swcfg->scripts, script, next);

		TRACE("Found Script: %s\n",
			script->fname);
	}
}

static void parse_uboot(parsertype p, void *cfg, struct swupdate_cfg *swcfg)
{
	void *setting, *elem;
	int count, i;
	struct uboot_var *uboot;

	setting = find_node(p, cfg, "uboot", swcfg);

	if (setting == NULL)
		return;

	count = get_array_length(p, setting);
	for(i = (count - 1); i >= 0; --i) {
		elem = get_elem_from_idx(p, setting, i);

		if (!elem)
			continue;

		if(!(exist_field_string(p, elem, "name")))
			continue;

		uboot = (struct uboot_var *)calloc(1, sizeof(struct uboot_var));
		if (!uboot) {
			ERROR( "No memory: malloc failed\n");
			return;
		}

		GET_FIELD_STRING(p, elem, "name", uboot->varname);
		GET_FIELD_STRING(p, elem, "value", uboot->value);
		TRACE("U-Boot var: %s = %s\n",
			uboot->varname,
			uboot->value);

		LIST_INSERT_HEAD(&swcfg->uboot, uboot, next);
	}
}

static void parse_images(parsertype p, void *cfg, struct swupdate_cfg *swcfg)
{
	void *setting, *elem;
	int count, i;
	struct img_type *image;

	setting = find_node(p, cfg, "images", swcfg);

	if (setting == NULL)
		return;

	count = get_array_length(p, setting);

	for(i = 0; i < count; ++i) {
		elem = get_elem_from_idx(p, setting, i);

		if (!elem)
			continue;

		if(!(exist_field_string(p, elem, "filename")))
			continue;

		image = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!image) {
			ERROR( "No memory: malloc failed\n");
			return;
		}

		GET_FIELD_STRING(p, elem, "filename", image->fname);
		GET_FIELD_STRING(p, elem, "volume", image->volname);
		GET_FIELD_STRING(p, elem, "device", image->device);
		GET_FIELD_STRING(p, elem, "type", image->type);

		/* if the handler is not explicit set, try to find the right one */
		if (!strlen(image->type)) {
			if (strlen(image->volname))
				strcpy(image->type, "ubivol");
			else if (strlen(image->device))
				strcpy(image->type, "raw");
		}

		get_field(p, elem, "compressed", &image->compressed);

		TRACE("Found %sImage: %s in %s : %s for handler %s\n",
			image->compressed ? "compressed " : "",
			image->fname,
			strlen(image->volname) ? "volume" : "device",
			strlen(image->volname) ? image->volname : image->device,
			strlen(image->type) ? image->type : "NOT FOUND"
			);

		LIST_INSERT_HEAD(&swcfg->images, image, next);
	}
}

static void parse_files(parsertype p, void *cfg, struct swupdate_cfg *swcfg)
{
	void *setting, *elem;
	int count, i;
	struct img_type *file;

	setting = find_node(p, cfg, "files", swcfg);

	if (setting == NULL)
		return;

	count = get_array_length(p, setting);
	for(i = 0; i < count; ++i) {
		elem = get_elem_from_idx(p, setting, i);

		if (!elem)
			continue;

		if(!(exist_field_string(p, elem, "filename")))
			continue;

		file = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!file) {
			ERROR( "No memory: malloc failed\n");
			return;
		}

		GET_FIELD_STRING(p, elem, "filename", file->fname);
		GET_FIELD_STRING(p, elem, "path", file->path);
		GET_FIELD_STRING(p, elem, "device", file->device);
		GET_FIELD_STRING(p, elem, "filesystem", file->filesystem);
		GET_FIELD_STRING(p, elem, "type", file->type);
		if (!strlen(file->type)) {
			strcpy(file->type, "rawfile");
		}
		get_field(p, elem, "compressed", &file->compressed);
		TRACE("Found %sFile: %s --> %s (%s)\n",
			file->compressed ? "compressed " : "",
			file->fname,
			file->path,
			strlen(file->device) ? file->device : "ROOTFS");

		LIST_INSERT_HEAD(&swcfg->images, file, next);
	}
}

static void parser(parsertype p, void *cfg, struct swupdate_cfg *swcfg)
{

	get_hw_revision(&hardware);

	/* Now parse the single elements */
	parse_hw_compatibility(p, cfg, swcfg);
	parse_images(p, cfg, swcfg);
	parse_scripts(p, cfg, swcfg);
	parse_uboot(p, cfg, swcfg);
	parse_files(p, cfg, swcfg);

	/*
	 * Move the partitions at the beginning to be processed
	 * before other images
	 */
	parse_partitions(p, cfg, swcfg);
}

#ifdef CONFIG_LIBCONFIG
int parse_cfg (struct swupdate_cfg *swcfg, const char *filename)
{
	config_t cfg;
	const char *str;
	char node[128];
	parsertype p = LIBCFG_PARSER;

	memset(&cfg, 0, sizeof(cfg));
	config_init(&cfg);

	/* Read the file. If there is an error, report it and exit. */
	if(config_read_file(&cfg, filename) != CONFIG_TRUE) {
		printf("%s ", config_error_file(&cfg));
		printf("%d ", config_error_line(&cfg));
		printf("%s ", config_error_text(&cfg));

		fprintf(stderr, "%s:%d - %s\n", config_error_file(&cfg),
			config_error_line(&cfg), config_error_text(&cfg));
		config_destroy(&cfg);
		ERROR(" ..exiting\n");
		return -1;
	}

	snprintf(node, sizeof(node), "%s.version",
			NODEROOT);

	if (!config_lookup_string(&cfg, node, &str)) {
		ERROR("Missing version in configuration file\n");
		return -1;
	} else {
		strncpy(swcfg->version, str, sizeof(swcfg->version));
		fprintf(stdout, "Version %s\n", swcfg->version);
	}

	parser(p, &cfg, swcfg);

	config_destroy(&cfg);

	return 0;
}
#endif

#ifdef CONFIG_JSON
int parse_json(struct swupdate_cfg *swcfg, const char *filename)
{
	int fd, ret;
	struct stat stbuf;
	unsigned int size;
	char *string;
	json_object *cfg;
	parsertype p = JSON_PARSER;

	/* Read the file. If there is an error, report it and exit. */
	ret = stat(filename, &stbuf);

	if (ret)
		return -EBADF;

	size = stbuf.st_size;
	string = (char *)malloc(size);
	if (!string)
		return -ENOMEM;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		free(string);
		return -EBADF;
	}

	ret = read(fd, string, size);
	close(fd);

	cfg = json_tokener_parse(string);
	if (!cfg) {
		ERROR("JSON File corrupted\n");
		free(string);
		return -1;
	}

	parser(p, cfg, swcfg);

	json_object_put(cfg);

	free(string);

	return 0;
}
#endif
