/*
 * (C) Copyright 2013-2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/ioctl.h>
#ifdef __FreeBSD__
#include <sys/disk.h>
// the ioctls are almost identical except for the name, just alias it
#define BLKGETSIZE64 DIOCGMEDIASIZE
#else
#include <linux/fs.h>
#endif

#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <libgen.h>

#include "swupdate_image.h"
#include "handler.h"
#include "util.h"

void raw_image_handler(void);
void raw_file_handler(void);

/**
 * Handle write protection for block devices
 *
 * Automatically remove write protection for block devices if:
 * - The device name starts with /dev/
 * - The device is a block device
 * - A corresponding ro flag e.g. /sys/class/block/mmcblk0boot0/force_ro is available
 * - The force_ro flag can be opened writeable
 */
static int blkprotect(struct img_type *img, bool on)
{
	char abs_path[PATH_MAX];
	const char c_sys_path[] = "/sys/class/block/%s/force_ro";
	const char c_unprot_char = '0';
	const char c_prot_char = '1';
	int ret = 0;  // 0 means OK nothing to do, 1 OK changed protection mode, negative means error
	int ret_int = 0;
	ssize_t ret_ss;
	char *sysfs_path = NULL;
	int fd_force_ro;
	struct stat sb;
	char current_prot;

	if (strncmp("/dev/", img->device, 5) != 0) {
		return ret;
	}

	if (stat(img->device, &sb) == -1) {
		TRACE("stat for device %s failed: %s", img->device, strerror(errno));
		return ret;
	}
	if(!S_ISBLK(sb.st_mode)) {
		return ret;
	}

	/* If given, traverse symlink and convert to absolute path */
	if (realpath(img->device, abs_path) == NULL) {
		ret = -errno;
		goto blkprotect_out;
	}

	ret_int = asprintf(&sysfs_path, c_sys_path, abs_path + 5);  /* remove "/dev/" from device path */
	if(ret_int < 0) {
		ret = -ENOMEM;
		goto blkprotect_out;
	}

	if (access(sysfs_path, W_OK) == -1) {
		goto blkprotect_out;
	}

	// There is a ro flag, the device needs to be protected or unprotected
	fd_force_ro = open(sysfs_path, O_RDWR);
	if (fd_force_ro == -1) {
		ret = -EBADF;
		goto blkprotect_out;
	}

	ret_ss = read(fd_force_ro, &current_prot, 1);
	if (ret_ss == 1) {
		char requested_prot = (on ? c_prot_char : c_unprot_char);
		if (requested_prot != current_prot) {
			ret_ss = write(fd_force_ro, &requested_prot, 1);
			if(ret_ss == 1) {
				TRACE("Device %s: changed force_ro to %c", img->device, requested_prot);
				ret = 1;
			} else {
				ret = -EIO;
			}
		}
	} else {
		ret = -EIO;
	}
	if (ret < 0) {
		TRACE("Device %s: changing force_ro mode failed!", img->device);
	}

	close(fd_force_ro);

blkprotect_out:
	if(sysfs_path)
		free(sysfs_path);
	return ret;
}

static int install_raw_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	int ret;
	int fdout;

	int prot_stat = blkprotect(img, false);
	if (prot_stat < 0)
		return prot_stat;

	fdout = open(img->device, O_RDWR);
	if (fdout < 0) {
		TRACE("Device %s cannot be opened: %s",
			img->device, strerror(errno));
		return -ENODEV;
	}
#if defined(__FreeBSD__)
	ret = copyimage(&fdout, img, copy_write_padded);
#else
	ret = copyimage(&fdout, img, NULL);
#endif

	if (prot_stat == 1) {
		fsync(fdout);  // At least with Linux 4.14 data are not automatically flushed before ro mode is enabled
		blkprotect(img, true);  // no error handling, keep ret from copyimage
	}

	close(fdout);
	return ret;
}

static int install_raw_file(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	char path[255];
	char tmp_path[255];
	int fdout = -1;
	int ret = -1;
	int cleanup_ret = 0;
	int use_mount = (strlen(img->device) && strlen(img->filesystem)) ? 1 : 0;
	char* DATADST_DIR = alloca(strlen(get_tmpdir())+strlen(DATADST_DIR_SUFFIX)+1);
	sprintf(DATADST_DIR, "%s%s", get_tmpdir(), DATADST_DIR_SUFFIX);

	if (strlen(img->path) == 0) {
		ERROR("Missing path attribute");
		return -1;
	}

	if (use_mount) {
		ret = swupdate_mount(img->device, DATADST_DIR, img->filesystem);
		if (ret) {
			ERROR("Device %s with filesystem %s cannot be mounted: %s",
				img->device, img->filesystem, strerror(errno));
			return -1;
		}

		if (snprintf(path, sizeof(path), "%s%s",
					 DATADST_DIR, img->path) >= (int)sizeof(path)) {
			ERROR("Path too long: %s%s", DATADST_DIR, img->path);
			goto cleanup;
		}
	} else {
		if (snprintf(path, sizeof(path), "%s", img->path) >= (int)sizeof(path)) {
			ERROR("Path too long: %s", img->path);
			return -1;
		}
	}

	if (strtobool(dict_get_value(&img->properties, "atomic-install"))) {
		if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path)) {
			ERROR("Temp path too long: %s.tmp", img->path);
			ret = -1;
			goto cleanup;
		}
	}
	else {
		snprintf(tmp_path, sizeof(tmp_path), "%s", path);
	}
	TRACE("Installing file %s on %s", img->fname, tmp_path);

	if (strtobool(dict_get_value(&img->properties, "create-destination"))) {
		TRACE("Creating path %s", path);
		ret = mkpath(dirname(strdupa(path)), 0755);
		if (ret < 0) {
			ERROR("I cannot create path %s: %s", path, strerror(errno));
			goto cleanup;
		}
	}

	fdout = openfileoutput(tmp_path);
	if (fdout < 0) {
		ret = -1;
		goto cleanup;
	}
	if (!img_check_free_space(img, fdout)) {
		ret = -ENOSPC;
		goto cleanup;
	}

	ret = copyimage(&fdout, img, NULL);
	if (ret < 0) {
		ERROR("Error copying extracted file");
		goto cleanup;
	}

	if (fsync(fdout)) {
		ERROR("Error writing %s to disk: %s", tmp_path, strerror(errno));
		ret = -1;
		goto cleanup;
	}

	close(fdout);
	fdout = 0;

	if (strtobool(dict_get_value(&img->properties, "atomic-install"))) {
		TRACE("Renaming file %s to %s", tmp_path, path);
		if (rename(tmp_path, path)) {
			ERROR("Error renaming %s to %s: %s", tmp_path, path, strerror(errno));
			ret = -1;
			goto cleanup;
		}
	}

	ret = 0;

cleanup:
	if (fdout > 0)
		close(fdout);

	if (use_mount) {
		cleanup_ret = swupdate_umount(DATADST_DIR);
		if (cleanup_ret)
			WARN("Can't unmount path %s: %s", DATADST_DIR, strerror(errno));
	}

	return (ret ? ret : cleanup_ret);
}

__attribute__((constructor))
void raw_image_handler(void)
{
	register_handler("raw", install_raw_image,
				IMAGE_HANDLER, NULL);
}

	__attribute__((constructor))
void raw_file_handler(void)
{
	register_handler("rawfile", install_raw_file,
				FILE_HANDLER, NULL);
}
