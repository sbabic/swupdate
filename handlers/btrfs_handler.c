/*
 * Copyright (C) 2023
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <errno.h>
#include <stdio.h>
#include <util.h>
#include <unistd.h>
#include <blkid/blkid.h>
#include <btrfsutil.h>
#include "progress.h"
#include "swupdate_image.h"
#include "handler.h"

typedef enum {
	BTRFS_UNKNOWN,
	BTRFS_CREATE_SUBVOLUME,
	BTRFS_DELETE_SUBVOLUME
} btrfs_op_t;

void btrfs_handler(void);

static int btrfs(struct img_type *img,
		      void __attribute__ ((__unused__)) *data)
{
	int ret = 0;
	btrfs_op_t op;
	enum btrfs_util_error btrfs_error;

	char *subvol_path = dict_get_value(&img->properties, "path");
	char *cmd = dict_get_value(&img->properties, "command");
	char *globalpath;
	char *mountpoint = NULL;

	op = IS_STR_EQUAL(cmd, "create") ? BTRFS_CREATE_SUBVOLUME :
		IS_STR_EQUAL(cmd, "delete") ? BTRFS_DELETE_SUBVOLUME : BTRFS_UNKNOWN;

	if (op == BTRFS_UNKNOWN) {
		ERROR("Wrong operation of btrfs filesystem: %s", cmd);
		return -EINVAL;
	}
	bool tomount = strtobool(dict_get_value(&img->properties, "mount"));
	if (tomount) {
		if (!strlen(img->device)) {
			ERROR("btrfs must be mounted, no device set");
			return -EINVAL;
		}
		DEBUG("Try to mount %s as BTRFS", mountpoint);
		mountpoint = swupdate_temporary_mount(MNT_DATA, img->device, "btrfs");
		if (!mountpoint) {
			ERROR("%s cannot be mounted with btrfs", img->device);
			return -1;
		}
		globalpath = alloca(strlen(mountpoint) + strlen(subvol_path) + 2);
		globalpath = strcat(mountpoint, subvol_path);
	} else
		globalpath = subvol_path;

	DEBUG("%s subvolume %s...", (op == BTRFS_CREATE_SUBVOLUME) ? "Creating" : "Deleting", subvol_path);
	switch (op) {
	case BTRFS_CREATE_SUBVOLUME:
		btrfs_error = btrfs_util_create_subvolume(globalpath, 0, NULL, NULL);
		break;
	case BTRFS_DELETE_SUBVOLUME:
		btrfs_error = btrfs_util_delete_subvolume(globalpath, BTRFS_UTIL_DELETE_SUBVOLUME_RECURSIVE);
		break;
	default:
		btrfs_error = BTRFS_UTIL_ERROR_FS_INFO_FAILED;
		break;
	}

	if (btrfs_error != BTRFS_UTIL_OK) {
		ERROR("BTRFS %s failed with btrfs error : %s", cmd,
			btrfs_util_strerror(btrfs_error));
		ret = -1;
	}

	if (tomount) {
		/*
		 * btrfs needs some time after creating a subvolume,
		 * so just delay here
		 */
		sleep(1);
		if (mountpoint) {
			swupdate_umount(mountpoint);
		}
	}
	/*
	 * Declare that handler has finished
	 */
	swupdate_progress_update(100);

	return ret;
}

__attribute__((constructor))
void btrfs_handler(void)
{
	register_handler("btrfs", btrfs,
			 PARTITION_HANDLER | NO_DATA_HANDLER, NULL);
}
