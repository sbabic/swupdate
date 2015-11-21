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
 * Foundation, Inc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>

#include "generated/autoconf.h"
#include "bsdqueue.h"
#include "globals.h"
#include "util.h"
#include "swupdate.h"
#include "installer.h"
#include "flash.h"
#include "handler.h"
#include "cpiohdr.h"
#include "parsers.h"
#include "fw_env.h"

/*
 * Extract all scripts from a list from the image
 * and save them on the filesystem to be executed later
 */
static int extract_script(int fd, struct imglist *head, const char *dest)
{
	struct img_type *script;
	int fdout;

	LIST_FOREACH(script, head, next) {
		if (script->provided == 0) {
			ERROR("Required script %s not found in image",
				script->fname);
			return -1;
		}

		snprintf(script->extract_file, sizeof(script->extract_file), "%s%s",
				dest, script->fname);

		fdout = openfileoutput(script->extract_file);
		extract_next_file(fd, fdout, script->offset, 0);
		close(fdout);
	}
	return 0;
}

static int prepare_uboot_script(struct swupdate_cfg *cfg, const char *script)
{
	int fd;
	int ret = 0;
	struct uboot_var *ubootvar;
	char buf[MAX_UBOOT_SCRIPT_LINE_LENGTH];

	fd = openfileoutput(script);
	if (fd < 0)
		return -1;

	LIST_FOREACH(ubootvar, &cfg->uboot, next) {
		snprintf(buf, sizeof(buf), "%s %s\n",
			ubootvar->varname,
			ubootvar->value);
		if (write(fd, buf, strlen(buf)) != strlen(buf)) {
			  TRACE("Error saving temporary file");
			  ret = -1;
			  break;
		}
	}
	close(fd);
	return ret;
}

static int update_uboot_env(void)
{
	int ret = 0;

#ifdef CONFIG_UBOOT
	TRACE("Updating U-boot environment");
	ret = fw_parse_script((char *)UBOOT_SCRIPT);
	if (ret < 0)
		ERROR("Error updating U-Boot environment");
#endif
	return ret;
}

int install_single_image(struct img_type *img)
{
	struct installer_handler *hnd;
	int ret;

	hnd = find_handler(img);
	if (!hnd) {
		TRACE("Image Type %s not supported", img->type);
		return -1;
	}
	TRACE("Found installer for stream %s %s", img->fname, hnd->desc);

	/* TODO : check callback to push results / progress */
	ret = hnd->installer(img, hnd->data);
	if (ret != 0) {
		TRACE("Installer for %s not successful !",
			hnd->desc);
	}

	return ret;
}

/*
 * streamfd: file descriptor if it is required to extract
 *           images from the stream (update from file)
 * extract : boolean, true to enable extraction
 */

int install_images(struct swupdate_cfg *sw, int fdsw, int fromfile)
{
	int ret;
	struct img_type *img;
	char filename[64];
	struct filehdr fdh;
	struct stat buf;

	/* Extract all scripts, preinstall scripts must be run now */
	if (fromfile) {
		ret = extract_script(fdsw, &sw->scripts, TMPDIR);
		if (ret) {
			ERROR("extracting script to TMPDIR failed");
			return ret;
		}
	}

	/* Scripts must be run before installing images */
	ret = run_prepost_scripts(sw, PREINSTALL);
	if (ret) {
		ERROR("execute preinstall scripts failed");
		return ret;
	}

	/* Update u-boot environment */
	ret = prepare_uboot_script(sw, UBOOT_SCRIPT);
	if (ret) {
		return ret;
	}


	LIST_FOREACH(img, &sw->images, next) {
		/*
		 *  If image is flagged to be installed from stream
		 *  it  was already installed by loading the
		 *  .swu image and it is skipped here.
		 */
		if (img->install_directly)
			continue;

		if (!fromfile) {
			snprintf(filename, sizeof(filename), "%s%s", TMPDIR, img->fname);

			ret = stat(filename, &buf);
			if (ret) {
				TRACE("%s not found or wrong", filename);
				return -1;
			}
			img->size = buf.st_size;

			img->fdin = open(filename, O_RDONLY);
			if (img->fdin < 0) {
				ERROR("Image %s cannot be opened",
				img->fname);
				return -1;
			}
		} else {
			if (extract_img_from_cpio(fdsw, img->offset, &fdh) < 0)
				return -1;
			img->size = fdh.size;
			img->checksum = fdh.chksum;
			img->fdin = fdsw;
		}

		ret = install_single_image(img);

		if (!fromfile)
			close(img->fdin);

		if (ret)
			return ret;

	}

	ret = run_prepost_scripts(sw, POSTINSTALL);
	if (ret) {
		ERROR("execute postinstall scripts failed");
		return ret;
	}

	if (!LIST_EMPTY(&sw->uboot))
		ret = update_uboot_env();

	return ret;
}

int run_prepost_scripts(struct swupdate_cfg *sw, script_fn type)
{
	int ret;
	struct img_type *img;
	struct installer_handler *hnd;

	/* Scripts must be run before installing images */
	LIST_FOREACH(img, &sw->scripts, next) {
		if (!img->is_script)
			continue;
		hnd = find_handler(img);
		if (hnd) {
			ret = hnd->installer(img, &type);
			if (ret)
				return ret;
		}
	}

	return 0;
}


static void remove_sw_file(char __attribute__ ((__unused__)) *fname)
{
#ifndef CONFIG_NOCLEANUP
	/* yes, "best effort", the files need not necessarily exist */
	unlink(fname);
#endif
}

void cleanup_files(struct swupdate_cfg *software) {
	char fn[64];
	struct img_type *img;
	struct uboot_var *ubootvar;
	struct hw_type *hw;

	LIST_FOREACH(img, &software->images, next) {
		if (img->fname[0]) {
			snprintf(fn, sizeof(fn), "%s%s", TMPDIR, img->fname);
			remove_sw_file(fn);
		}
		LIST_REMOVE(img, next);
		free(img);
	}
	LIST_FOREACH(img, &software->scripts, next) {
		if (img->fname[0]) {
			snprintf(fn, sizeof(fn), "%s%s", TMPDIR, img->fname);
			remove_sw_file(fn);
		}
		LIST_REMOVE(img, next);
		free(img);
	}
	LIST_FOREACH(ubootvar, &software->uboot, next) {
		LIST_REMOVE(ubootvar, next);
		free(ubootvar);
	}

	LIST_FOREACH(hw, &software->hardware, next) {
		LIST_REMOVE(hw, next);
		free(hw);
	}
	snprintf(fn, sizeof(fn), "%s%s", TMPDIR, SW_DESCRIPTION_FILENAME);
	remove_sw_file(fn);
}
