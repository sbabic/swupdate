/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/fs.h>


#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <libgen.h>

#include "swupdate.h"
#include "handler.h"
#include "util.h"

void raw_image_handler(void);
void raw_file_handler(void);
void raw_copyimage_handler(void);

static int install_raw_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	int ret;
	int fdout;

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

	close(fdout);
	return ret;
}

static int copy_raw_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	int ret;
	int fdout, fdin;
	struct dict_list *proplist;
	struct dict_list_elem *entry;
	uint32_t checksum;
	unsigned long offset = 0;
	size_t size;

	proplist = dict_get_list(&img->properties, "copyfrom");

	if (!proplist || !(entry = LIST_FIRST(proplist))) {
		ERROR("MIssing source device, no copyfrom property");
		return -EINVAL;
	}
	fdin = open(entry->value, O_RDONLY);
	if (fdin < 0) {
		TRACE("Device %s cannot be opened: %s",
			entry->value, strerror(errno));
		return -ENODEV;
	}

	if (ioctl(fdin, BLKGETSIZE64, &size) < 0) {
		ERROR("Cannot get size of %s", entry->value);
	}

	fdout = open(img->device, O_RDWR);
	if (fdout < 0) {
		TRACE("Device %s cannot be opened: %s",
			img->device, strerror(errno));
		close(fdin);
		return -ENODEV;
	}

	ret = copyfile(fdin,
			&fdout,
			size,
			&offset,
			0,
			0, /* no skip */
			0, /* no compressed */
			&checksum,
			0, /* no sha256 */
			0, /* no encrypted */
			NULL);

	close(fdout);
	return ret;
}

static int install_raw_file(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	char path[255];
	int fdout;
	int ret = 0;
	int use_mount = (strlen(img->device) && strlen(img->filesystem)) ? 1 : 0;
	char* DATADST_DIR = alloca(strlen(get_tmpdir())+strlen(DATADST_DIR_SUFFIX)+1);
	sprintf(DATADST_DIR, "%s%s", get_tmpdir(), DATADST_DIR_SUFFIX);
	char* make_path;

	if (strlen(img->path) == 0) {
		ERROR("Missing path attribute");
		return -1;
	}

	if (use_mount) {
		ret = swupdate_mount(img->device, DATADST_DIR, img->filesystem);
		if (ret) {
			ERROR("Device %s with filesystem %s cannot be mounted",
				img->device, img->filesystem);
			return -1;
		}

		if (snprintf(path, sizeof(path), "%s%s",
					 DATADST_DIR, img->path) >= (int)sizeof(path)) {
			ERROR("Path too long: %s%s", DATADST_DIR, img->path);
			return -1;
		}
	} else {
		if (snprintf(path, sizeof(path), "%s", img->path) >= (int)sizeof(path)) {
			ERROR("Path too long: %s", img->path);
			return -1;
		}
	}

	TRACE("Installing file %s on %s",
		img->fname, path);

	make_path = dict_get_value(&img->properties, "create-destination");

	if (make_path != NULL && strcmp(make_path, "true") == 0) {
		TRACE("Creating path %s", path);
		fdout = mkpath(dirname(strdupa(path)), 0755);
		if (fdout < 0) {
			ERROR("I cannot create path %s: %s", path, strerror(errno));
			return -1;
		}
	}

	fdout = openfileoutput(path);
	ret = copyimage(&fdout, img, NULL);
	if (ret< 0) {
		ERROR("Error copying extracted file");
	}
	close(fdout);

	if (use_mount) {
		swupdate_umount(DATADST_DIR);
	}

	return ret;
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

__attribute__((constructor))
void raw_copyimage_handler(void)
{
	register_handler("rawcopy", copy_raw_image,
				IMAGE_HANDLER | NO_DATA_HANDLER, NULL);
}
