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

void diskformat_handler(void);

/*
 * Checks if file system fstype already exists on device.
 * return 0 if not exists, 1 if exists, negative values on failure
 */
static int fs_exists(char *device, char *fstype)
{
	char buf[10];
	const char *value = buf;
	size_t len;
	blkid_probe pr;
	int ret = 0;

	pr = blkid_new_probe_from_filename(device);

	if (!pr) {
		ERROR("%s: failed to create libblkid probe",
		      device);
		return -EFAULT;
	}

	while (blkid_do_probe(pr) == 0) {
		if (blkid_probe_lookup_value(pr, "TYPE", &value, &len)) {
			ERROR("blkid_probe_lookup_value failed");
			ret = -EFAULT;
			break;
		}

		if (!strncmp(value, fstype, sizeof(buf))) {
			ret = 1;
			break;
		}
	}
	blkid_free_probe(pr);

	return ret;
}

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

	if (force != NULL && strcmp(force, "true") == 0) {
		; /* Skip file system exists check */
	} else {
		/* Check if file system exists */
		ret = fs_exists(img->device, fstype);

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
	return ret;
}

__attribute__((constructor))
void diskformat_handler(void)
{
	register_handler("diskformat", diskformat,
			 PARTITION_HANDLER | NO_DATA_HANDLER, NULL);
}
