/*
 * (C) Copyright 2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
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
#include "uboot.h"
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
		ERROR("Error opening U-Boot lock file %s, %s", lockname, strerror(errno));
		return -1;
	}
	if (flock(lockfd, LOCK_EX) < 0) {
		ERROR("Error locking file %s, %s", lockname, strerror(errno));
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
		ERROR("Error: environment not initialized, %s", strerror(errno));
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
		ERROR("Error: environment not initialized, %s", strerror(errno));
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
		ERROR("Error opening U-Boot lock file %s, %s", lockname, strerror(errno));
		return -ENODEV;
	}

	ret = fw_parse_script((char *)filename, fw_env_opts);
	fw_env_close (fw_env_opts);
	unlock_uboot_env(lockfd);
	
	return ret;
}
