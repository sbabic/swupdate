/*
 * Copyright (C) 2023 Stefano Babic, stefano.babic@swupdate.org
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <blkid/blkid.h>
#include <uuid/uuid.h>
#include "util.h"
#include "fs_interface.h"
#include "pctl.h"

extern int mkfs_main(int argc, const char **argv);
int btrfs_mkfs(const char *device_name, const char __attribute__ ((__unused__)) *fstype)
{
	int fd, ret;
	const char *argv[3] = { "mkfs.btrfs", "-f", NULL };

	if (!device_name)
		return -EINVAL;

	fd = open(device_name, O_RDWR);

	if (fd < 0) {
		ERROR("%s cannot be opened", device_name);
		return -EINVAL;
	}
	close(fd);

	optind = 1;
	argv[2] = device_name;

#if defined(CONFIG_BTRFS_FILESYSTEM_USELIBMKFS)
	int argc;
	argc = 3;
	ret = run_function_background(mkfs_main, argc, (char **)argv);
#else
	char *cmd;

	if (asprintf(&cmd, "%s %s %s\n", argv[0], argv[1], argv[2]) == -1) {
		ERROR("Error allocating memory");
		return -ENOMEM;
	}

	ret = run_system_cmd(cmd);

#endif
	return ret;
}
