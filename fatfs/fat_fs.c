/*
 * Copyright (C) 2021 Weidmueller Interface GmbH & Co. KG
 * Roland Gaudig <roland.gaudig@weidmueller.com>
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <errno.h>
#include <stddef.h>

#include <fatfs_interface.h>
#include <swupdate.h>
#include <util.h>

#include "ff.h"


int fat_mkfs(char *device_name)
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
