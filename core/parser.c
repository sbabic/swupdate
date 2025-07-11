/*
 * (C) Copyright 2013
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
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
		 * Skip handler with no data because there is no image
		 * associated for this type
		 */
		if ( !(get_handler_mask(image) & NO_DATA_HANDLER) &&
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

struct swupdate_type_cfg *swupdate_find_update_type(struct swupdate_type_list *list, const char *name)
{
	struct swupdate_type_cfg *type_cfg = NULL;

	LIST_FOREACH(type_cfg, list, next) {
		if (!strcmp(type_cfg->type_name, name)) {
			return type_cfg;
		}
	}
	return NULL;
}

int parse(struct swupdate_cfg *sw, const char *descfile)
{
	int ret = -1;
	parser_fn current;
#ifdef CONFIG_SIGNED_IMAGES
	char *sigfile;

	sigfile = malloc(strlen(descfile) + strlen(".sig") + 1);
	if (!sigfile)
		return -ENOMEM;
	strcpy(sigfile, descfile);
	strcat(sigfile, ".sig");

	ret = swupdate_verify_file(sw->dgst, sigfile, descfile,
				   sw->forced_signer_name);
	free(sigfile);

	if (ret)
		return ret;

#endif
	char *errors[ARRAY_SIZE(parsers)] = {0};

	for (unsigned int i = 0; i < ARRAY_SIZE(parsers); i++) {
		current = parsers[i];

		ret = current(sw, descfile, &errors[i]);

		if (ret == 0)
			break;
	}

	if (ret != 0) {
		for (unsigned int i = 0; i < ARRAY_SIZE(parsers); i++) {
			if (errors[i] != NULL) {
				ERROR("%s", errors[i]);
				free(errors[i]);
			}
		}
		ERROR("no parser available to parse " SW_DESCRIPTION_FILENAME "!");
		return ret;
	}

	for (unsigned int i = 0; i < ARRAY_SIZE(parsers); i++)
		if (errors[i] != NULL)
			free(errors[i]);

	ret = check_handler_list(&sw->scripts, SCRIPT_HANDLER, IS_SCRIPT, "scripts");
	ret |= check_handler_list(&sw->images, IMAGE_HANDLER | FILE_HANDLER, IS_IMAGE_FILE,
					"images / files");
	ret |= check_handler_list(&sw->images, PARTITION_HANDLER, IS_PARTITION,
					"partitions");
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
	 * Check if a an Update Type is set and
	 * load the configuration
	 */
	struct swupdate_type_cfg *update_type;
	if (!strnlen(sw->update_type_name, sizeof(sw->update_type_name) - 1)) {
		if (sw->update_type_required) {
			ERROR("Update Type is mandatory but it was not set");
			return -EINVAL;
		} else
			strlcpy(sw->update_type_name,
				"default",
				sizeof(sw->update_type_name));
	}
	update_type = swupdate_find_update_type(&sw->swupdate_types, sw->update_type_name);
	if (!update_type) {
		ERROR("Requested Update of Type %s but it is not configured", sw->update_type_name);
		return -EINVAL;
	}

	sw->update_type = update_type;

	/*
	 * If downgrading is not allowed, convert
	 * versions in numbers to be compared and check to get a
	 * newer version
	 */
	if (sw->update_type->no_downgrading) {
		if (compare_versions(sw->version, sw->update_type->minimum_version) < 0) {
			ERROR("No downgrading allowed: new version %s < installed %s",
				sw->version, sw->update_type->minimum_version);
			return -EPERM;
		}
	}

	/*
	 * Check if update is allowed until a chosen version, convert
	 * versions in numbers to be compared and check to get a
	 * newer version
	 */
	if (sw->update_type->check_max_version) {
		if (compare_versions(sw->version, sw->update_type->maximum_version) > 0) {
			ERROR("Max version set: new version %s > max allowed %s",
				sw->version, sw->update_type->maximum_version);
			return -EPERM;
		}
	}

	/*
	 * If reinstalling is not allowed, compare
	 * version strings
	 */
	if (sw->update_type->no_reinstalling) {

		if (strcmp(sw->version, sw->update_type->current_version) == 0) {
			ERROR("No reinstalling allowed: new version %s == installed %s",
				sw->version, sw->update_type->current_version);
			return -EPERM;
		}
	}

	/*
	 * Compute the total number of installer
	 * to initialize the progress bar
	 */
	unsigned int totalsteps = count_elem_list(&sw->images) +
					2 * count_elem_list(&sw->scripts);
	swupdate_progress_init(totalsteps);

	TRACE("Number of found artifacts: %d", count_elem_list(&sw->images));
	TRACE("Number of scripts: %d", count_elem_list(&sw->scripts));
	TRACE("Number of steps to be run: %d", totalsteps);


	/*
	 * Send the version string as first message to progress interface
	 */
	char *versioninfo;
	if (asprintf(&versioninfo, "{\"VERSION\" : \"%s\"}", sw->version) == ENOMEM_ASPRINTF)
		ERROR("OOM sending version info");
	else {
		swupdate_progress_info(RUN, NONE, versioninfo);
		free(versioninfo);
	}

	return ret;
}
