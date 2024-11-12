/*
 * Copyright (C) 2023
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <unistd.h>
#include <blkid/blkid.h>
#include <pthread.h>
#include <btrfsutil.h>
#include "progress.h"
#include "swupdate_image.h"
#include "handler.h"
#include <util.h>
#include <pctl.h>
#include "handler_helpers.h"

typedef enum {
	BTRFS_UNKNOWN,
	BTRFS_CREATE_SUBVOLUME,
	BTRFS_DELETE_SUBVOLUME
} btrfs_op_t;

#define BTRFS_MOUNT(mount, device, mnt, subvol, path) do {  \
	if (mount) { \
		if (!btrfs_mount(device, &mnt)) { \
			ERROR("%s cannot be mounted with btrfs", device); \
			return -1; \
		} \
		path = swupdate_strcat(2, mnt, subvol); \
	} else \
		path = strdup(subvol); \
} while (0)

/*
 * This is just a wrapper as some parms are fixed
 * (filesystem, etc)
 */
static bool btrfs_mount(const char *device, char **mnt)
{
	if (!mnt)
		return false;

	*mnt = swupdate_temporary_mount(MNT_DATA, device, "btrfs");
	if (!*mnt) {
		ERROR("%s cannot be mounted with btrfs", device);
		return false;
	}

	return true;
}

static int btrfs(struct img_type *img,
		      void __attribute__ ((__unused__)) *data)
{
	int ret = 0;
	btrfs_op_t op;
	enum btrfs_util_error btrfs_error;

	char *subvol_path = dict_get_value(&img->properties, "path");
	char *cmd = dict_get_value(&img->properties, "command");
	bool tomount = strtobool(dict_get_value(&img->properties, "mount"));
	char *globalpath = NULL;
	char *mountpoint = NULL;

	op = IS_STR_EQUAL(cmd, "create") ? BTRFS_CREATE_SUBVOLUME :
		IS_STR_EQUAL(cmd, "delete") ? BTRFS_DELETE_SUBVOLUME : BTRFS_UNKNOWN;

	if (op == BTRFS_UNKNOWN) {
		ERROR("Wrong operation of btrfs filesystem: %s", cmd);
		return -EINVAL;
	}

	BTRFS_MOUNT(tomount, img->device, mountpoint, subvol_path, globalpath);

	DEBUG("%s subvolume %s...", (op == BTRFS_CREATE_SUBVOLUME) ? "Creating" : "Deleting", subvol_path);
	switch (op) {
	case BTRFS_CREATE_SUBVOLUME:
		if (strtobool(dict_get_value(&img->properties, "create-destination"))) {
			char *parent = dirname(strdupa(globalpath));
			DEBUG("Creating subvolume destination directory: %s", parent);
			ret = mkpath(parent, 0755);
			if (ret < 0) {
				ERROR("Failed to create subvolume destination directory %s: %s",
					parent, strerror(errno));
				ret = -1;
				goto cleanup;
			}
		}
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

cleanup:
	if (tomount && mountpoint) {
		/*
		 * btrfs needs some time after creating a subvolume,
		 * so just delay here
		 */
		sleep(1);
		swupdate_umount(mountpoint);
	}
	/*
	 * Declare that handler has finished
	 */
	swupdate_progress_update(100);

	free(globalpath);

	return ret;
}

static int install_btrfs_snapshot(struct img_type *img,
		      void __attribute__ ((__unused__)) *data)
{
	int ret = 0;
	struct bgtask_handle btrfs_handle;
	char *subvol_path = dict_get_value(&img->properties, "path");
	bool tomount = strtobool(dict_get_value(&img->properties, "mount"));
	const char *btrfscmd = dict_get_value(&img->properties, "btrfs-cmd");
	char *globalpath = NULL;
	char *mountpoint = NULL;

	/*
	 * if no path for the command is found, the handler assumes that
	 * btrfs is in standard path
	 */
	if (!btrfscmd)
		btrfscmd = "/usr/bin/btrfs ";

	BTRFS_MOUNT(tomount, img->device, mountpoint, subvol_path, globalpath);

	btrfs_handle.cmd = btrfscmd;

	/*
	 * Note: btrfs tool writes to stderr instead of stdout
	 * and SWUpdate will intercept and show the output as error
	 * even if they are not. Just redirect it to stdout to drop
	 * error messages
	 */
	btrfs_handle.parms = swupdate_strcat(3, " receive ", globalpath, " 2>&1");
	free(globalpath);
	btrfs_handle.img = img;

	ret = bgtask_handler(&btrfs_handle);

	if (tomount && mountpoint)
		swupdate_umount(mountpoint);
	free(btrfs_handle.parms);
	return ret;
}

__attribute__((constructor))
static void btrfs_handler(void)
{
	register_handler("btrfs", btrfs,
			 PARTITION_HANDLER | NO_DATA_HANDLER, NULL);
}

__attribute__((constructor))
static void btrfs_receive_handler(void)
{
	register_handler("btrfs-receive", install_btrfs_snapshot,
				IMAGE_HANDLER, NULL);
}
