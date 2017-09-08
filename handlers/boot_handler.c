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

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mtd/mtd-user.h>
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
	char buf[64];

	char filename[64];
	struct stat statbuf;

	const char* TMPDIR = get_tmpdir();
	if (snprintf(filename, sizeof(filename), "%s%s", TMPDIR,
		     img->fname) >= (int)sizeof(filename)) {
		ERROR("Path too long: %s%s", TMPDIR, img->fname);
		return -1;
	}
	ret = stat(filename, &statbuf);
	if (ret) {
		fdout = openfileoutput(filename);
		/*
		 * Bootloader environment is set inside sw-description
		 * there is no hash but sw-description was already verified
		 */
		ret = copyimage(&fdout, img, NULL);
		close(fdout);
	}

	ret = bootloader_apply_list(filename);
	if (ret < 0)
		snprintf(buf, sizeof(buf), "Error setting bootloader environment");
	else
		snprintf(buf, sizeof(buf), "Bootloader environment updated");

	notify(RUN, RECOVERY_NO_ERROR, buf);

	return ret;
}

__attribute__((constructor))
static void uboot_handler(void)
{
	register_handler("uboot", install_boot_environment, NULL);
}
__attribute__((constructor))
static void boot_handler(void)
{
	register_handler("bootloader", install_boot_environment, NULL);
}
