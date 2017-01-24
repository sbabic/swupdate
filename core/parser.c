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

int parse(struct swupdate_cfg *sw, const char *descfile)
{
	int ret = -1;
	int i;
	parser_fn current;
#ifdef CONFIG_SIGNED_IMAGES
	char *sigfile;
#endif

	for (i = 0; i < ARRAY_SIZE(parsers); i++) {
		current = parsers[i];

		ret = current(sw, descfile);

		if (ret == 0)
			break;
	}

	if (ret != 0) {
		ERROR("no parser available to parse " SW_DESCRIPTION_FILENAME "!\n");
		return ret;
	}

	struct img_type *item;
	if (!LIST_EMPTY(&sw->scripts)) {
		LIST_FOREACH(item, &sw->scripts, next)
		{
			if (!find_handler(item)) {
				ERROR("feature '%s' required for script "
				      "'%s' in %s is absent!",
				      item->type, item->fname,
				      SW_DESCRIPTION_FILENAME);
				return -1;
			}
		}
	}
	if (!LIST_EMPTY(&sw->images)) {
		LIST_FOREACH(item, &sw->images, next)
		{
			if (!find_handler(item)) {
				ERROR("feature '%s' required for image "
				      "'%s' in %s is absent!",
				      item->type, item->fname,
				      SW_DESCRIPTION_FILENAME);
				return -1;
			}
		}
	}
	struct img_type item_uboot = {.type = "uboot"};
	if (!LIST_EMPTY(&sw->uboot) && !find_handler(&item_uboot)) {
		ERROR("feature 'uboot' absent but %s has 'uboot' section!",
		      SW_DESCRIPTION_FILENAME);
		return -1;
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
#endif

	/*
	 * Compute the total number of installer
	 * to initialize the progress bar
	 */
	swupdate_progress_init(count_elem_list(&sw->images));

	return ret;
}
