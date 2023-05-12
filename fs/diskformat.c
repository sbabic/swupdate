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

#if defined(CONFIG_EXT_FILESYSTEM)
static inline int ext_mkfs_short(const char *device_name, const char *fstype)
{
	return ext_mkfs(device_name, fstype, 0, NULL);
}
#endif

struct supported_filesystems {
	const char *fstype;
	int (*mkfs)(const char *device_name, const char *fstype);
};

static struct supported_filesystems fs[] = {
#if defined(CONFIG_FAT_FILESYSTEM)
	{"vfat", fat_mkfs},
#endif
#if defined(CONFIG_EXT_FILESYSTEM)
	{"ext2", ext_mkfs_short},
	{"ext3", ext_mkfs_short},
	{"ext4", ext_mkfs_short},
#endif
#if defined(CONFIG_BTRFS_FILESYSTEM)
	{"btrfs", btrfs_mkfs},
#endif
};

/*
 * Checks if file system fstype already exists on device.
 * return 0 if not exists, 1 if exists, negative values on failure
 */

char *diskformat_fs_detect(char *device)
{
	const char *value;
	char *s = NULL;
	size_t len;
	blkid_probe pr;

	pr = blkid_new_probe_from_filename(device);

	if (!pr) {
		ERROR("%s: failed to create libblkid probe",
			  device);
		return NULL;
	}

	while (blkid_do_probe(pr) == 0) {
		if (blkid_probe_lookup_value(pr, "TYPE", &value, &len)) {
			ERROR("blkid_probe_lookup_value failed");
			break;
		}

		if (len > 0) {
			s = strndup(value, len);
			break;
		}
	}
	blkid_free_probe(pr);

	return s;
}

int diskformat_fs_exists(char *device, char *fstype)
{
	int ret = 0;
	char *filesystem = diskformat_fs_detect(device);

	if (filesystem) {
		ret = !strcmp(fstype, filesystem);
	}

	free(filesystem);
	return ret;
}

int diskformat_mkfs(char *device, char *fstype)
{
	int index;
	int ret = 0;

	if (!device || !fstype) {
		ERROR("Uninitialized pointer as device/fstype argument");
		return -EINVAL;
	}

	for (index = 0; index < ARRAY_SIZE(fs); index++) {
		if (!strcmp(fs[index].fstype, fstype))
			break;
	}
	if (index >= ARRAY_SIZE(fs)) {
		ERROR("%s file system type not supported.", fstype);
		return -EINVAL;
	}

	TRACE("Creating %s file system on %s", fstype, device);
	ret = fs[index].mkfs(device, fstype);

	if (ret) {
		ERROR("creating %s file system on %s failed. %d",
		      fstype, device, ret);
		return -EFAULT;
	}

	return ret;
}
