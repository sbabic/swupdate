/*
 * (C) Copyright 2013
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
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

static int check_handler_list(struct imglist *list, unsigned int allowedmask,
				const char *desc)
{
	struct img_type *item;
	int ret;
	if (!LIST_EMPTY(list)) {
		LIST_FOREACH(item, list, next)
		{
			ret = check_handler(item, allowedmask, desc);

			if (ret < 0)
				return ret;
		}
	}

	return 0;
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
		ERROR("no parser available to parse " SW_DESCRIPTION_FILENAME "!\n");
		return ret;
	}

	ret = check_handler_list(&sw->scripts, SCRIPT_HANDLER, "scripts");
	ret |= check_handler_list(&sw->images, IMAGE_HANDLER | FILE_HANDLER,
					"images / files");
	ret |= check_handler_list(&sw->partitions, PARTITION_HANDLER,
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
	 * Compute the total number of installer
	 * to initialize the progress bar
	 */
	swupdate_progress_init(count_elem_list(&sw->images));

	return ret;
}
