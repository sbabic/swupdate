/*
 * (C) Copyright 2015-2016
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
#include "parsers.h"
#include "swupdate_dict.h"

#define MODULE_NAME	"PARSER"

#define NODEROOT (!strlen(CONFIG_LIBCONFIGROOT) ? \
			"software" : CONFIG_LIBCONFIGROOT)

#ifdef CONFIG_LIBCONFIG
static config_setting_t *find_node_libconfig(config_t *cfg,
					const char *field, struct swupdate_cfg *swcfg)
{
	config_setting_t *setting;
	struct hw_type *hardware;

	char node[1024];

	if (!field)
		return NULL;

	hardware = &swcfg->hw;

	if (strlen(swcfg->running_mode) && strlen(swcfg->software_set)) {
		/* Try with both software set and board name */
		if (strlen(hardware->boardname)) {
			snprintf(node, sizeof(node), "%s.%s.%s.%s.%s",
				NODEROOT,
				hardware->boardname,
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
	if (strlen(hardware->boardname)) {
		snprintf(node, sizeof(node), "%s.%s.%s",
			NODEROOT,
			hardware->boardname,
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

#endif

#ifdef CONFIG_JSON
static json_object *find_node_json(json_object *root, const char *node,
			struct swupdate_cfg *swcfg)
{
	json_object *jnode = NULL;
	const char *simple_nodes[] = {node, NULL};
	struct hw_type *hardware;

	hardware = &swcfg->hw;

	if (strlen(swcfg->running_mode) && strlen(swcfg->software_set)) {
		if (strlen(hardware->boardname)) {
			const char *nodes[] = {hardware->boardname, swcfg->software_set,
					       swcfg->running_mode, node, NULL};
			jnode = find_json_recursive_node(root, nodes);
			if (jnode)
				return jnode;
		} else {
			const char *nodes[] = {swcfg->software_set, swcfg->running_mode,
					       node, NULL};
			jnode = find_json_recursive_node(root, nodes);
			if (jnode)
				return jnode;
		}
	}

	if (strlen(hardware->boardname)) {
		const char *nodes[] = {hardware->boardname, node, NULL};
		jnode = find_json_recursive_node(root, nodes);
		if (jnode)
			return jnode;
	}

	return find_json_recursive_node(root, simple_nodes);
}
#endif

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
	/*
	 * Parse in reverse order, so that the partitions are processed
	 * by LIST_HEAD() in the same order as they are found in sw-description
	 */
	for(i = (count - 1); i >= 0; --i) {
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

		if (!strlen(partition->volname) || !strlen(partition->device)) {
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

		/*
		 * Check for mandatory field
		 */
		if(!(exist_field_string(p, elem, "filename"))) {
			TRACE("Script entry without filename field, skipping..");
			continue;
		}

		script = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!script) {
			ERROR( "No memory: malloc failed\n");
			return;
		}

		GET_FIELD_STRING(p, elem, "filename", script->fname);
		GET_FIELD_STRING(p, elem, "type", script->type);
		GET_FIELD_STRING(p, elem, "data", script->type_data);
		get_hash_value(p, elem, script->sha256);

		get_field(p, elem, "encrypted", &script->is_encrypted);

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
	char name[32];
	char value[255];

	setting = find_node(p, cfg, "uboot", swcfg);

	if (setting == NULL)
		return;

	count = get_array_length(p, setting);
	for(i = (count - 1); i >= 0; --i) {
		elem = get_elem_from_idx(p, setting, i);

		if (!elem)
			continue;

		/*
		 * Check for mandatory field
		 */
		if(!(exist_field_string(p, elem, "name"))) {
			TRACE("U-Boot entry without variable name field, skipping..");
			continue;
		}


		/*
		 * Call directly get_field_string with size 0
		 * to let allocate the place for the strings
		 */
		GET_FIELD_STRING(p, elem, "name", name);
		GET_FIELD_STRING(p, elem, "value", value);
		dict_set_value(&swcfg->uboot, name, value);

		TRACE("U-Boot var: %s = %s\n",
			name,
			dict_get_value(&swcfg->uboot, name));

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

		/*
		 * Check for mandatory field
		 */
		if(!(exist_field_string(p, elem, "filename"))) {
			TRACE("Image entry without filename field, skipping..");
			continue;
		}

		image = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!image) {
			ERROR( "No memory: malloc failed\n");
			return;
		}

		GET_FIELD_STRING(p, elem, "name", image->id.name);
		GET_FIELD_STRING(p, elem, "version", image->id.version);
		GET_FIELD_STRING(p, elem, "filename", image->fname);
		GET_FIELD_STRING(p, elem, "volume", image->volname);
		GET_FIELD_STRING(p, elem, "device", image->device);
		GET_FIELD_STRING(p, elem, "type", image->type);
		GET_FIELD_STRING(p, elem, "data", image->type_data);
		get_hash_value(p, elem, image->sha256);

		/* if the handler is not explicit set, try to find the right one */
		if (!strlen(image->type)) {
			if (strlen(image->volname))
				strcpy(image->type, "ubivol");
			else if (strlen(image->device))
				strcpy(image->type, "raw");
		}

		get_field(p, elem, "compressed", &image->compressed);
		get_field(p, elem, "installed-directly", &image->install_directly);
		get_field(p, elem, "install-if-different", &image->id.install_if_different);
		get_field(p, elem, "encrypted", &image->is_encrypted);

		TRACE("Found %sImage %s %s: %s in %s : %s for handler %s%s %s\n",
			image->compressed ? "compressed " : "",
			image->id.name,
			image->id.version,
			image->fname,
			strlen(image->volname) ? "volume" : "device",
			strlen(image->volname) ? image->volname : image->device,
			strlen(image->type) ? image->type : "NOT FOUND",
			image->install_directly ? " (installed from stream)" : "",
			(strlen(image->id.name) && image->id.install_if_different) ?
					"Version must be checked" : ""
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

		/*
		 * Check for mandatory field
		 */
		if(!(exist_field_string(p, elem, "filename"))) {
			TRACE("File entry without filename field, skipping..");
			continue;
		}

		file = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!file) {
			ERROR( "No memory: malloc failed\n");
			return;
		}

		GET_FIELD_STRING(p, elem, "name", file->id.name);
		GET_FIELD_STRING(p, elem, "version", file->id.version);
		GET_FIELD_STRING(p, elem, "filename", file->fname);
		GET_FIELD_STRING(p, elem, "path", file->path);
		GET_FIELD_STRING(p, elem, "device", file->device);
		GET_FIELD_STRING(p, elem, "filesystem", file->filesystem);
		GET_FIELD_STRING(p, elem, "type", file->type);
		GET_FIELD_STRING(p, elem, "data", file->type_data);
		get_hash_value(p, elem, file->sha256);

		if (!strlen(file->type)) {
			strcpy(file->type, "rawfile");
		}
		get_field(p, elem, "compressed", &file->compressed);
		get_field(p, elem, "installed-directly", &file->install_directly);
		get_field(p, elem, "install-if-different", &file->id.install_if_different);
		get_field(p, elem, "encrypted", &file->is_encrypted);
		TRACE("Found %sFile %s %s: %s --> %s (%s) %s\n",
			file->compressed ? "compressed " : "",
			file->id.name,
			file->id.version,
			file->fname,
			file->path,
			strlen(file->device) ? file->device : "ROOTFS",
			(strlen(file->id.name) && file->id.install_if_different) ?
					"Version must be checked" : "");

		LIST_INSERT_HEAD(&swcfg->images, file, next);
	}
}

static void parser(parsertype p, void *cfg, struct swupdate_cfg *swcfg)
{

	get_hw_revision(&swcfg->hw);

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
#else
int parse_cfg (struct swupdate_cfg __attribute__ ((__unused__)) *swcfg,
		const char __attribute__ ((__unused__)) *filename)
{
	return -1;
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
#else
int parse_json(struct swupdate_cfg __attribute__ ((__unused__)) *swcfg,
		const char __attribute__ ((__unused__)) *filename)
{
	return -1;
}
#endif
