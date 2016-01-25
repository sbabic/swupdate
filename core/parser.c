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

static parser_fn parsers[] = {
	parse_cfg,
	parse_json,
	parse_external
};

/*
 * Check that all images in a list have a valid hsh
 */
static int check_missing_hash(struct imglist *list)
{
	struct img_type *image;

	LIST_FOREACH(image, list, next) {
		if (!IsValidHash(image->sha256)) {
			ERROR("Hash not set for %s Type %s",
				image->fname,
				image->type);
			return -EINVAL;
		}
	}

	return 0;
}

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

	if (ret != 0)
		return ret;

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
	 * If the software must verified, all images
	 * must have a valid hash to be checked
	 */
	if (check_missing_hash(&sw->images) ||
		check_missing_hash(&sw->scripts))
		ret = -EINVAL;
#endif

	return ret;
}
