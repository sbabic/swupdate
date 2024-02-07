/*
 * Copyright (C) 2021 Weidmueller Interface GmbH & Co. KG
 * Roland Gaudig <roland.gaudig@weidmueller.com>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <errno.h>
#include <stdio.h>
#include <util.h>
#include <handler.h>
#include <blkid/blkid.h>
#include <fs_interface.h>
#include "progress.h"
#include "swupdate_image.h"

void diskformat_handler(void);

static int diskformat(struct img_type *img,
		      void __attribute__ ((__unused__)) *data)
{
	int ret = 0;

	if (!strlen(img->device)) {
		ERROR("diskpart handler requires setting \"device\" attribute");
		return -EINVAL;
	}

	char *fstype = dict_get_value(&img->properties, "fstype");

	if (!fstype) {
		ERROR("diskpart handler requires setting \"fstype\" attribute");
		return -EINVAL;
	}

	char *force = dict_get_value(&img->properties, "force");

	if (force != NULL && strtobool(force)) {
		; /* Skip file system exists check */
	} else {
		/* Check if file system exists */
		ret = diskformat_fs_exists(img->device, fstype);

		if (ret < 0)
			return ret;

		if (ret) {
			TRACE("Found %s file system on %s, skip mkfs",
			      fstype, img->device);
			return 0;
		}
	}

	/* File system does not exist, create new file system */
	ret = diskformat_mkfs(img->device, fstype);

	/*
	 * Declare that handler has finished
	 */
	swupdate_progress_update(100);

	return ret;
}

__attribute__((constructor))
void diskformat_handler(void)
{
	register_handler("diskformat", diskformat,
			 PARTITION_HANDLER | NO_DATA_HANDLER, NULL);
}
