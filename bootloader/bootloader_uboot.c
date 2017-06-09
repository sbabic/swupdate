/*
 * (C) Copyright 2017
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

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <dirent.h>
#include "generated/autoconf.h"
#include "fw_env.h"
#include "util.h"
#include "bootloader.h"

struct env_opts *fw_env_opts = &(struct env_opts) {
	.config_file = (char *)CONFIG_UBOOT_FWENV
};

/*
 * The lockfile is the same as defined in U-Boot for
 * the fw_printenv utilities
 */
static const char *lockname = "/var/lock/fw_printenv.lock";
static int lock_uboot_env(void)
{
	int lockfd = -1;
	lockfd = open(lockname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (lockfd < 0) {
		ERROR("Error opening U-Boot lock file %s\n", lockname);
		return -1;
	}
	if (flock(lockfd, LOCK_EX) < 0) {
		ERROR("Error locking file %s\n", lockname);
		close(lockfd);
		return -1;
	}

	return lockfd;
}

static void unlock_uboot_env(int lock)
{
	flock(lock, LOCK_UN);
	close(lock);
}

int bootloader_env_set(const char *name, const char *value)
{
	int lock = lock_uboot_env();
	int ret;

	if (lock < 0)
		return -1;

	if (fw_env_open (fw_env_opts)) {
		fprintf (stderr, "Error: environment not initialized\n");
		unlock_uboot_env(lock);
		return -1;
	}
	fw_env_write ((char *)name, (char *)value);
	ret = fw_env_flush(fw_env_opts);
	fw_env_close (fw_env_opts);

	unlock_uboot_env(lock);

	return ret;
}

int bootloader_env_unset(const char *name)
{
	return bootloader_env_set(name, "");
}

char *bootloader_env_get(const char *name)
{
	int lock;
	char *value = NULL;
	char *var;

	lock = lock_uboot_env();
	if (lock < 0)
		return NULL;

	if (fw_env_open (fw_env_opts)) {
		ERROR("Error: environment not initialized\n");
		unlock_uboot_env(lock);
		return NULL;
	}

	var = fw_getenv((char *)name);
	if (var)
		value = strdup(var);

	fw_env_close (fw_env_opts);

	unlock_uboot_env(lock);

	return value;
}

int bootloader_apply_list(const char *filename)
{
	int lockfd;
	int ret;

	lockfd = lock_uboot_env();
	if (lockfd < 0) {
		ERROR("Error opening U-Boot lock file %s\n", lockname);
		return -ENODEV;
	}

	ret = fw_parse_script((char *)filename, fw_env_opts);
	fw_env_close (fw_env_opts);
	unlock_uboot_env(lockfd);
	
	return ret;
}
