/*
 * (C) Copyright 2015
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * See file CREDITS for list of people who contributed to this
 * project.
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
#include <errno.h>
#include <sys/queue.h>
#include <json-c/json.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include "autoconf.h"
#include "util.h"
#include "swupdate.h"
#include "parsers.h"

#define MODULE_NAME	"JSONPARSER"

#define GET_FIELD(e, name, d) \
	get_field_string(e, name, d, sizeof(d))

static struct hw_type hardware;

static json_object *get_board_node(json_object *jobj)
{
	json_object *jnode = NULL;

	char target[1024];
	if (strlen(hardware.boardname)) {
		snprintf(target, sizeof(target), "%s",
			hardware.boardname);
		jnode = json_object_object_get(jobj, target);
	}
	return jnode ? jnode : jobj;
}

static void get_field_string(json_object *e, const char *path, char *dest, size_t n)
{
	const char *str;
	json_object *node;

	node = json_object_object_get(e, path);
	if (node && (json_object_get_type(node) == json_type_string)) {
		str = json_object_get_string(node);
		strncpy(dest, str, n);
	}
}

static void get_field(json_object *e, const char *path, void *dest)
{
	json_object *fld = NULL;
	enum json_type type;

	fld = json_object_object_get(e, path);
	if (fld) {
		type = json_object_get_type(fld);
		switch (type) {
		case json_type_boolean:
			*(unsigned int *)dest = json_object_get_boolean(fld);
			break;
		case json_type_int:
			*(unsigned int *)dest = json_object_get_int(fld);
			break;
		case json_type_string:
			strcpy(dest, json_object_get_string(fld));
			break;
		case json_type_double:
			*(double *)dest = json_object_get_double(fld);
			break;
		default:
			break;
		}
	}
}

#ifdef CONFIG_HW_COMPATIBILITY
/*
 * Check if the software can run on the hardware
 */
static int parse_hw_compatibility(json_object *jobj, struct swupdate_cfg *swcfg)
{
	json_object *node, *hw;
	int count, i;
	const char *s;
	struct hw_type *hwrev;

	node = json_object_object_get(jobj, "hardware-compatibility");
	if (node == NULL) {
		ERROR("HW compatibility not found\n");
		return -1;
	}

	count = json_object_array_length(node); /*Getting the length of the array*/

	for(i = 0; i < count; ++i) {
		hw = json_object_array_get_idx(node, i);

		s = json_object_get_string(hw);
		if (!s)
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
static int parse_hw_compatibility(json_object __attribute__ ((__unused__))  *jobj, struct swupdate_cfg __attribute__ ((__unused__)) *swcfg)
{
	return 0;
}
#endif

static void parse_partitions(json_object * jobj, struct swupdate_cfg *swcfg)
{
	json_object *node, *elem;
	int count, i;
	struct img_type *partition;

	node = json_object_object_get(jobj, "partitions");

	if (node == NULL)
		return;

	count = json_object_array_length(node); /*Getting the length of the array*/
	for(i = 0; i < count; ++i) {
		elem = json_object_array_get_idx(node, i);

		if (!elem)
			continue;

		partition = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!partition) {
			ERROR("No memory: malloc failed\n");
			return;
		}
		GET_FIELD(elem, "name", partition->volname);
		GET_FIELD(elem, "device", partition->device);
		strncpy(partition->type, "ubipartition", sizeof(partition->type));
		partition->is_partitioner = 1;

		partition->provided = 1;

		if (!partition->volname || !partition->device) {
			ERROR("Partition incompleted in description file");
			return;
		}

		get_field(elem, "size", &partition->partsize);

		TRACE("Partition: %s new size %lld bytes\n",
			partition->volname,
			partition->partsize);

		LIST_INSERT_HEAD(&swcfg->images, partition, next);
	}
}

static void parse_scripts(json_object * jobj, struct swupdate_cfg *swcfg)
{
	json_object *node, *elem;
	int count, i;
	struct img_type *script;

	node = json_object_object_get(jobj, "scripts");

	if (node == NULL)
		return;

	count = json_object_array_length(node); /*Getting the length of the array*/

	/*
	 * Scan the scripts in reserve order to maintain
	 * the same order using LIST_INSERT_HEAD
	 */
	for(i = (count - 1); i >= 0; --i) {
		elem = json_object_array_get_idx(node, i);

		if (!elem)
			continue;

		if (!json_object_object_get(elem, "filename"))
			continue;

		script = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!script) {
			ERROR( "No memory: malloc failed\n");
			return;
		}

		GET_FIELD(elem, "filename", script->fname);
		GET_FIELD(elem, "type", script->type);

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

static void parse_uboot(json_object * jobj, struct swupdate_cfg *swcfg)
{
	json_object *node, *elem;
	int count, i;
	struct uboot_var *uboot;

	node = json_object_object_get(jobj, "uboot");

	if (node == NULL)
		return;

	count = json_object_array_length(node); /*Getting the length of the array*/
	for(i = (count - 1); i >= 0; --i) {
		elem = json_object_array_get_idx(node, i);

		if (!elem)
			continue;

		if (!json_object_object_get(elem, "name"))
			continue;

		uboot = (struct uboot_var *)calloc(1, sizeof(struct uboot_var));
		if (!uboot) {
			ERROR( "No memory: malloc failed\n");
			return;
		}

		GET_FIELD(elem, "name", uboot->varname);
		GET_FIELD(elem, "value", uboot->value);
		TRACE("U-Boot var: %s = %s\n",
			uboot->varname,
			uboot->value);

		LIST_INSERT_HEAD(&swcfg->uboot, uboot, next);
	}
}

static void parse_images(json_object * jobj, struct swupdate_cfg *swcfg)
{
	json_object *node, *elem;
	int count, i;
	struct img_type *image;

	node = json_object_object_get(jobj, "images");

	if (node == NULL)
		return;

	count = json_object_array_length(node); /*Getting the length of the array*/

	for(i = 0; i < count; ++i) {
		elem = json_object_array_get_idx(node, i);

		if (!elem)
			continue;

		if (!json_object_object_get(elem, "filename"))
			continue;

		image = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!image) {
			ERROR( "No memory: malloc failed\n");
			return;
		}

		GET_FIELD(elem, "filename", image->fname);
		GET_FIELD(elem, "volume", image->volname);
		GET_FIELD(elem, "device", image->device);
		GET_FIELD(elem, "type", image->type);

		/* if the handler is not explicit set, try to find the right one */
		if (!strlen(image->type)) {
			if (strlen(image->volname))
				strcpy(image->type, "ubivol");
			else if (strlen(image->device))
				strcpy(image->type, "raw");
		}

		get_field(elem, "compressed", &image->compressed);

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

static void parse_files(json_object *jobj, struct swupdate_cfg *swcfg)
{
	json_object *node, *elem;
	int count, i;
	struct img_type *file;

	node = json_object_object_get(jobj, "files");

	if (node == NULL)
		return;

	count = json_object_array_length(node); /*Getting the length of the array*/
	for(i = 0; i < count; ++i) {
		elem = json_object_array_get_idx(node, i);

		if (!elem)
			continue;

		if (!json_object_object_get(elem, "filename"))
			continue;

		file = (struct img_type *)calloc(1, sizeof(struct img_type));
		if (!file) {
			ERROR( "No memory: malloc failed\n");
			return;
		}

		GET_FIELD(elem, "filename", file->fname);
		GET_FIELD(elem, "path", file->path);
		GET_FIELD(elem, "device", file->device);
		GET_FIELD(elem, "filesystem", file->filesystem);
		strcpy(file->type, "rawfile");
		get_field(elem, "compressed", &file->compressed);

		TRACE("Found %sFile: %s --> %s (%s)\n",
			file->compressed ? "compressed " : "",
			file->fname,
			file->path,
			strlen(file->device) ? file->device : "ROOTFS");

		LIST_INSERT_HEAD(&swcfg->images, file, next);
	}
}

int parse_json(struct swupdate_cfg *swcfg, const char *filename)
{
	int fd, ret;
	struct stat stbuf;
	unsigned int size;
	char *string;
	json_object *cfg;

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

	get_hw_revision(&hardware);
	cfg = get_board_node(cfg);

	/* Now parse the single elements */
	parse_hw_compatibility(cfg, swcfg);
	parse_images(cfg, swcfg);
	parse_scripts(cfg, swcfg);
	parse_uboot(cfg, swcfg);
	parse_files(cfg, swcfg);

	/*
	 * Move the partitions at the beginning to be processed
	 * before other images
	 */
	parse_partitions(cfg, swcfg);

	json_object_put(cfg);

	free(string);

	return 0;
}
