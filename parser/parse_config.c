/*
 * (C) Copyright 2012
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
#include <libconfig.h>
#include <sys/types.h>
#include <sys/queue.h>
#include "autoconf.h"
#include "util.h"
#include "swupdate.h"
#include "parsers.h"

#define MODULE_NAME	"CFG"

#define GET_FIELD(e, name, d) \
	get_field(e, name, d, sizeof(d))

static struct hw_type hardware;

static const config_setting_t *get_setting(config_t *cfg,
	const char *field)
{
	const config_setting_t *setting;

	char node[1024];
	if (field && strlen(hardware.boardname)) {
		snprintf(node, sizeof(node), "software.%s.%s",
			hardware.boardname,
			field);
		setting = config_lookup(cfg, node);
		if (setting)
			return setting;
	}
	/* Fall back without board entry */
	snprintf(node, sizeof(node), "software.%s",
		field);
	return config_lookup(cfg, node);
}

static void get_field(const config_setting_t *e, const char *path, char *dest, size_t n)
{
	const char *str;
	if (config_setting_lookup_string(e, path, &str))
		strncpy(dest, str, n);
}

#ifdef CONFIG_HW_COMPATIBILITY
/*
 * Check if the software can run on the hardware
 */
static int parse_hw_compatibility(config_t *cfg, struct swupdate_cfg *swcfg)
{
	const config_setting_t *setting, *hw;
	int count, i;
	const char *s;
	struct hw_type *hwrev;

	setting = get_setting(cfg, "hardware-compatibility");
	if (setting == NULL) {
		ERROR("HW compatibility not found\n");
		return -1;
	}

	count = config_setting_length(setting);

	for(i = 0; i < count; ++i) {
		hw = config_setting_get_elem(setting, i);

		s = config_setting_get_string(hw);
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
static int parse_hw_compatibility(config_t __attribute__ ((__unused__))  *cfg,
		struct swupdate_cfg __attribute__ ((__unused__)) *swcfg)
{
	return 0;
}
#endif

static void parse_partitions(config_t *cfg, struct swupdate_cfg *swcfg)
{
	const config_setting_t *setting, *elem;
	int count, i;
	struct img_type *partition;

	setting = get_setting(cfg, "partitions");

	if (setting == NULL)
		return;

	count = config_setting_length(setting);
	for(i = 0; i < count; ++i) {
		elem = config_setting_get_elem(setting, i);

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

		if (!config_setting_lookup_int64(elem, "size", &partition->partsize)) {
			ERROR("Size not set for partition %s\n", partition->volname);
			free (partition);
			return;
		}

		TRACE("Partition: %s new size %lld bytes\n",
			partition->volname,
			partition->partsize);
 
		LIST_INSERT_HEAD(&swcfg->images, partition, next);
	}
}

static void parse_scripts(config_t *cfg, struct swupdate_cfg *swcfg)
{
	const config_setting_t *setting, *elem;
	int count, i;
	struct img_type *script;
	const char *str;

	setting = get_setting(cfg, "scripts");

	if (setting == NULL)
		return;

	count = config_setting_length(setting);
	/*
	 * Scan the scripts in reserve order to maintain
	 * the same order using LIST_INSERT_HEAD
	 */
	for(i = (count - 1); i >= 0; --i) {
		elem = config_setting_get_elem(setting, i);

		if (!elem)
			continue;

		if(!(config_setting_lookup_string(elem, "filename", &str)))
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

		TRACE("Found Script: %s %s\n",
			script->fname,
			str);
	}
}

static void parse_uboot(config_t *cfg, struct swupdate_cfg *swcfg)
{
	const config_setting_t *setting, *elem;
	int count, i;
	struct uboot_var *uboot;
	const char *str;

	setting = get_setting(cfg, "uboot");

	if (setting == NULL)
		return;

	count = config_setting_length(setting);
	for(i = (count - 1); i >= 0; --i) {
		elem = config_setting_get_elem(setting, i);

		if (!elem)
			continue;

		if(!(config_setting_lookup_string(elem, "name", &str)))
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

static void parse_images(config_t *cfg, struct swupdate_cfg *swcfg)
{
	const config_setting_t *setting, *elem;
	int count, i;
	struct img_type *image;
	const char *str;

	setting = get_setting(cfg, "images");

	if (setting == NULL)
		return;

	count = config_setting_length(setting);

	for(i = 0; i < count; ++i) {
		elem = config_setting_get_elem(setting, i);

		if (!elem)
			continue;

		if(!(config_setting_lookup_string(elem, "filename", &str)))
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

		if (!config_setting_lookup_bool(elem, "compressed", &image->compressed))
			image->compressed = 0;

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

static void parse_files(config_t *cfg, struct swupdate_cfg *swcfg)
{
	const config_setting_t *setting, *elem;
	int count, i;
	struct img_type *file;
	const char *str;

	setting = get_setting(cfg, "files");

	if (setting == NULL)
		return;

	count = config_setting_length(setting);
	for(i = 0; i < count; ++i) {
		elem = config_setting_get_elem(setting, i);

		if (!elem)
			continue;

		if(!(config_setting_lookup_string(elem, "filename", &str)))
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
		if (config_setting_lookup_bool(elem,
			"compressed", &file->compressed) == CONFIG_FALSE)
			file->compressed = 0;
		TRACE("Found %sFile: %s --> %s (%s)\n",
			file->compressed ? "compressed " : "",
			file->fname,
			file->path,
			strlen(file->device) ? file->device : "ROOTFS");
 
		LIST_INSERT_HEAD(&swcfg->images, file, next);
	}
}

int parse_cfg (struct swupdate_cfg *swcfg, const char *filename)
{
	config_t cfg;
	const char *str;

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

	if (!config_lookup_string(&cfg, "software.version", &str)) {
		ERROR("Missing version in configuration file\n");
		return -1;
	} else {
		strncpy(swcfg->version, str, sizeof(swcfg->version));
		fprintf(stdout, "Version %s\n", swcfg->version);
	}

	get_hw_revision(&hardware);

	/* Now parse the single elements */
	parse_hw_compatibility(&cfg, swcfg);
	parse_images(&cfg, swcfg);
	parse_scripts(&cfg, swcfg);
	parse_uboot(&cfg, swcfg);
	parse_files(&cfg, swcfg);

	/*
	 * Move the partitions at the beginning to be processed
	 * before other images
	 */
	parse_partitions(&cfg, swcfg);

	config_destroy(&cfg);

	return 0;
}
