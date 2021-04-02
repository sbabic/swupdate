/*
 * (C) Copyright 2020
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * This handler does not install, but checks that
 * there is not a filesystem with a specific UUID on the device.
 * This is useful in case the bootloader choses the partition to be
 * started with FS-UUID.
 */
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <blkid/blkid.h>
#include <sys/types.h>
#include "swupdate.h"
#include "handler.h"
#include "util.h"

void uniqueuuid_handler(void);

static int uniqueuuid(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	struct dict_list *uuids;
	struct dict_list_elem *uuid;
	blkid_cache cache = NULL;
	blkid_dev_iterate	iter;
	blkid_dev		dev;
	int ret = 0;

	uuids = dict_get_list(&img->properties, "fs-uuid");
	if (!uuids) {
		ERROR("Check for uuids runs, but not uuid given !");
		return -EINVAL;
	}

	blkid_get_cache(&cache, NULL);

	blkid_probe_all(cache);

	LIST_FOREACH(uuid, uuids, next) {
		iter = blkid_dev_iterate_begin(cache);
		blkid_dev_set_search(iter, "UUID", uuid->value);

		while (blkid_dev_next(iter, &dev) == 0) {
			dev = blkid_verify(cache, dev);
			if (!dev)
				continue;
			ERROR("UUID=%s not unique on %s !", uuid->value,
				blkid_dev_devname(dev));
			ret = -EAGAIN;
		}
		blkid_dev_iterate_end(iter);
	}

	return ret;
}

__attribute__((constructor))
void uniqueuuid_handler(void)
{
	register_handler("uniqueuuid", uniqueuuid,
				PARTITION_HANDLER | NO_DATA_HANDLER, NULL);
}

