/*
 * Copyright (C) 2021 Weidmueller Interface GmbH & Co. KG
 * Roland Gaudig <roland.gaudig@weidmueller.com>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <errno.h>
#include <stddef.h>

#include <fs_interface.h>
#include <util.h>

#include "ff.h"


int fat_mkfs(const char *device_name, const char __attribute__ ((__unused__)) *fstype)
{
	if (fatfs_init(device_name))
		return -1;

	void* working_buffer = malloc(FF_MAX_SS);

	if (!working_buffer) {
		fatfs_release();
		return -ENOMEM;
	}

	MKFS_PARM mkfs_parm = {
		.fmt = FM_ANY | FM_SFD,
		.au_size = 0,
		.align = 0,
		.n_fat = 0,
		.n_root = 0
	};

	FRESULT result = f_mkfs("", &mkfs_parm, working_buffer, FF_MAX_SS);
	free(working_buffer);

	if (result != FR_OK) {
		fatfs_release();
		return -1;
	}

	fatfs_release();
	return 0;
}

int fat_set_label(const char *device_name, const char *label)
{
	int ret = 0;

	if (fatfs_init(device_name)) {
		return -1;
	}

	FATFS fs;
	/* initialize the library (without mounting anything!) */
	FRESULT result = f_mount(&fs, "", 0);
	if (result != FR_OK) {
		ERROR("Failed to initialize fatfs library (reason: %d)", result);
		ret = -1;
		goto finish;
	}

	/* try to read existing label and do nothing if it matches our label */
	char current[12]; /* 11 is the maximum length of a FAT label, +1 for null-termination */
	if (f_getlabel(device_name, &current[0], NULL) == FR_OK) {
		current[sizeof(current) - 1] = '\0'; /* make sure it's null-terminated */
		DEBUG("%s has fslabel '%s'", device_name, current);
		if (!strcasecmp(label, current)) {
			TRACE("Current fslabel '%s' matches new label, skipping setlabel", current);
			goto finish;
		}
	} else {
		DEBUG("Failed to read existing fslabel");
	}
	TRACE("Setting FAT fslabel '%s' on %s", label, device_name);
	if (f_setlabel(label) != FR_OK) {
		ERROR("%s: failed to set fslabel", device_name);
		ret = -1;
	}

finish:
	f_unmount("");
	fatfs_release();
	return ret;
}
