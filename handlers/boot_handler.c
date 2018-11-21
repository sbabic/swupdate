/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "generated/autoconf.h"
#include "swupdate.h"
#include "handler.h"
#include "util.h"
#include "bootloader.h"

static void uboot_handler(void);
static void boot_handler(void);

static int install_boot_environment(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	int ret;
	int fdout;

	char filename[64];
	struct stat statbuf;

	if (snprintf(filename, sizeof(filename), "%s%s", get_tmpdirscripts(),
		     img->fname) >= (int)sizeof(filename)) {
		ERROR("Path too long: %s%s", get_tmpdirscripts(),
			 img->fname);
		return -1;
	}
	ret = stat(filename, &statbuf);
	if (ret) {
		fdout = openfileoutput(filename);
		ret = copyimage(&fdout, img, NULL);
		close(fdout);
	}

	ret = bootloader_apply_list(filename);
	if (ret != 0) {
		ERROR("Error setting bootloader environment");
	} else {
		TRACE("Bootloader environment from %s updated", img->fname);
	}

	return ret;
}

__attribute__((constructor))
static void uboot_handler(void)
{
	register_handler("uboot", install_boot_environment,
				IMAGE_HANDLER | BOOTLOADER_HANDLER, NULL);
}
__attribute__((constructor))
static void boot_handler(void)
{
	register_handler("bootloader", install_boot_environment,
				IMAGE_HANDLER | BOOTLOADER_HANDLER, NULL);
}
