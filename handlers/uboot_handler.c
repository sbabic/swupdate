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
#include "fw_env.h"
#include "util.h"

static void uboot_handler(void);

struct env_opts *fw_env_opts = &(struct env_opts) {
	.config_file = (char *)CONFIG_UBOOT_FWENV
};

static int install_uboot_environment(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	int ret;
	int fdout;
	char buf[64];
	int lockfd;

	char filename[64];
	struct stat statbuf;

	snprintf(filename, sizeof(filename), "%s%s", TMPDIR, img->fname);
	ret = stat(filename, &statbuf);
	if (ret) {
		fdout = openfileoutput(filename);
		/*
		 * U-Boot environment is set inside sw-description
		 * there is no hash but sw-description was already verified
		 */
		ret = copyimage(&fdout, img, NULL);
		close(fdout);
	}

	lockfd = lock_uboot_env();
	if (lockfd > 0)
		ret = fw_parse_script(filename, fw_env_opts);

	if (ret < 0)
		snprintf(buf, sizeof(buf), "Error setting U-Boot environment");
	else
		snprintf(buf, sizeof(buf), "U-Boot environment updated");

	notify(RUN, RECOVERY_NO_ERROR, buf);

	if (lockfd > 0)
		unlock_uboot_env(lockfd);

	return ret;

}

__attribute__((constructor))
static void uboot_handler(void)
{
	register_handler("uboot", install_uboot_environment, NULL);
}
