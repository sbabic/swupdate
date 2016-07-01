/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
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
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "swupdate.h"
#include "handler.h"
#include "util.h"

void raw_handler(void);
void raw_filecopy_handler(void);

static int install_raw_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	int ret;
	int fdout;

	fdout = open(img->device, O_RDWR);
	if (fdout < 0) {
		TRACE("Device %s cannot be opened: %s",
				img->device, strerror(errno));
		return -1;
	}
	
	ret = copyimage(fdout, img);

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

	if (strlen(img->path) == 0) {
		ERROR("Missing path attribute");
		return -1;
	}

	if (use_mount) {
		ret = mount(img->device, DATADST_DIR, img->filesystem, 0, NULL);
		if (ret) {
			ERROR("Device %s with filesystem %s cannot be mounted",
				img->device, img->filesystem);
			return -1;
		}

		snprintf(path, sizeof(path), "%s%s",
			DATADST_DIR, img->path);
	} else {
		snprintf(path, sizeof(path), "%s", img->path);
	}

	TRACE("Installing file %s on %s\n",
		img->fname, path);
	fdout = openfileoutput(path);
	ret = copyimage(fdout, img);
	if (ret< 0) {
		ERROR("Error copying extracted file\n");
	}
	close(fdout);

	if (use_mount) {
		umount(DATADST_DIR);
	}

	return ret;
}

__attribute__((constructor))
void raw_handler(void)
{
	register_handler("raw", install_raw_image, NULL);
}

	__attribute__((constructor))
void raw_filecopy_handler(void)
{
	register_handler("rawfile", install_raw_file, NULL);
}
