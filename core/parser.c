/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include "swupdate.h"
#include "parsers.h"
#include "sslapi.h"
#include "util.h"
#include "progress.h"
#include "handler.h"

static parser_fn parsers[] = {
	parse_cfg,
	parse_json,
	parse_external
};

typedef enum {
	IS_IMAGE_FILE,
	IS_SCRIPT,
	IS_PARTITION,
	IS_UNKNOWN
} IMGTYPE;

static inline IMGTYPE get_entry_type(struct img_type *img)
{
	if (!img->is_script && !img->is_partitioner)
		return IS_IMAGE_FILE;
	if (img->is_script)
		return IS_SCRIPT;
	if (img->is_partitioner)
		return IS_PARTITION;
	return IS_UNKNOWN;
}


#ifndef CONFIG_HASH_VERIFY
static int check_hash_absent(struct imglist *list)
{
	struct img_type *image;
	LIST_FOREACH(image, list, next) {
		if (strnlen((const char *)image->sha256, SHA256_HASH_LENGTH) > 0) {
			ERROR("hash verification not enabled but hash supplied for %s",
				  image->fname);
			return -EINVAL;
		}
	}
	return 0;
}
#endif

#ifdef CONFIG_SIGNED_IMAGES
/*
 * Check that all images in a list have a valid hash
 */
static int check_missing_hash(struct imglist *list)
{
	struct img_type *image;

	LIST_FOREACH(image, list, next) {
		/*
		 * Skip "ubipartition" because there is no image
		 * associated for this type
		 */
		if ( (strcmp(image->type, "ubipartition")) &&
				(!IsValidHash(image->sha256))) {
			ERROR("Hash not set for %s Type %s",
				image->fname,
				image->type);
			return -EINVAL;
		}
	}

	return 0;
}
#endif

static int check_handler(struct img_type *item, unsigned int mask, const char *desc)
{
	struct installer_handler *hnd;

	hnd = find_handler(item);
	if (!hnd) {
		ERROR("feature '%s' required for "
		      "'%s' in %s is absent!",
		      item->type, item->fname,
		      SW_DESCRIPTION_FILENAME);
		return -EINVAL;
	}

	if (!(hnd->mask & mask)) {
		ERROR("feature '%s' is not allowed for "
		      "'%s' in %s is absent!",
		      item->type, desc,
		      SW_DESCRIPTION_FILENAME);
		return -EINVAL;
	}

	return 0;
}

static int check_handler_list(struct imglist *list,
				unsigned int allowedmask,
				IMGTYPE type,
				const char *desc)
{
	struct img_type *item;
	int ret;
	if (!LIST_EMPTY(list)) {
		LIST_FOREACH(item, list, next)
		{
			if (get_entry_type(item) != type)
				continue;
			ret = check_handler(item, allowedmask, desc);

			if (ret < 0)
				return ret;
		}
	}

	return 0;
}

static int is_image_installed(struct swver *sw_ver_list,
				struct img_type *img)
{
	struct sw_version *swver;

	if (!sw_ver_list)
		return false;

	if (!strlen(img->id.name) || !strlen(img->id.version) ||
		!img->id.install_if_different)
		return false;

	LIST_FOREACH(swver, sw_ver_list, next) {
		/*
		 * Check if name and version are identical
		 */
		if (!strncmp(img->id.name, swver->name, sizeof(img->id.name)) &&
		    !strncmp(img->id.version, swver->version, sizeof(img->id.version))) {
			TRACE("%s(%s) already installed, skipping...",
				img->id.name,
				img->id.version);

			return true;
		}
	}

	return false;
}

/*
 * Remove the image if the same version is already installed
 */
static void remove_installed_image_list(struct imglist *img_list,
				struct swver *sw_ver_list)
{
	struct img_type *img;

	LIST_FOREACH(img, img_list, next) {
		if (is_image_installed(sw_ver_list, img)) {
			LIST_REMOVE(img, next);
			free_image(img);
		}
	}
}

int parse(struct swupdate_cfg *sw, const char *descfile)
{
	int ret = -1;
	parser_fn current;
#ifdef CONFIG_SIGNED_IMAGES
	char *sigfile;
#endif
	for (unsigned int i = 0; i < ARRAY_SIZE(parsers); i++) {
		current = parsers[i];

		ret = current(sw, descfile);

		if (ret == 0)
			break;
	}

	if (ret != 0) {
		ERROR("no parser available to parse " SW_DESCRIPTION_FILENAME "!");
		return ret;
	}

	ret = check_handler_list(&sw->scripts, SCRIPT_HANDLER, IS_SCRIPT, "scripts");
	ret |= check_handler_list(&sw->images, IMAGE_HANDLER | FILE_HANDLER, IS_IMAGE_FILE,
					"images / files");
	ret |= check_handler_list(&sw->images, PARTITION_HANDLER, IS_PARTITION,
					"partitions");
	ret |= check_handler_list(&sw->bootscripts, BOOTLOADER_HANDLER, IS_SCRIPT,
					"bootloader");
	if (ret)
		return -EINVAL;

	/*
	 *  Bootloader is slightly different, it has no image
	 *  but a list of variables
	 */
	struct img_type item_uboot = {.type = "uboot"};
	struct img_type item_bootloader = {.type = "bootenv"};
	if (!LIST_EMPTY(&sw->bootloader) &&
			(!find_handler(&item_uboot) &&
			 !find_handler(&item_bootloader))) {
		ERROR("bootloader support absent but %s has bootloader section!",
		      SW_DESCRIPTION_FILENAME);
		return -EINVAL;
	}

#ifdef CONFIG_SIGNED_IMAGES
	sigfile = malloc(strlen(descfile) + strlen(".sig") + 1);
	if (!sigfile)
		return -ENOMEM;
	strcpy(sigfile, descfile);
	strcat(sigfile, ".sig");

	ret = swupdate_verify_file(sw->dgst, sigfile, descfile);
	free(sigfile);

	if (ret)
		return ret;

	/*
	 * If the software must be verified, all images
	 * must have a valid hash to be checked
	 */
	if (check_missing_hash(&sw->images) ||
		check_missing_hash(&sw->scripts))
		ret = -EINVAL;
#else
#ifndef CONFIG_HASH_VERIFY
	if (check_hash_absent(&sw->images) ||
		check_hash_absent(&sw->scripts))
		ret = -EINVAL;
#endif
#endif

	/*
	 * If downgrading is not allowed, convert
	 * versions in numbers to be compared and check to get a
	 * newer version
	 */
	if (sw->globals.no_downgrading) {
		__u64 currentversion = version_to_number(sw->globals.current_version);
		__u64 newversion = version_to_number(sw->version);

		if (newversion < currentversion) {
			ERROR("No downgrading allowed: new version %s < installed %s",
				sw->version, sw->globals.current_version);
			return -EPERM;
		}
	}

	remove_installed_image_list(&sw->images, &sw->installed_sw_list);

	/*
	 * Compute the total number of installer
	 * to initialize the progress bar
	 */
	swupdate_progress_init(count_elem_list(&sw->images));

	return ret;
}
