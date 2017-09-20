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
#include <stdbool.h>
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
#include "handler.h"
#include "cpiohdr.h"
#include "parsers.h"
#include "bootloader.h"
#include "progress.h"

static int isImageInstalled(struct swver *sw_ver_list,
				struct img_type *img)
{
	struct sw_version *swver;

	if (!sw_ver_list)
		return false;

	if (!strlen(img->id.name) || !strlen(img->id.version) ||
		!img->id.install_if_different)
		return false;

	LIST_FOREACH(swver, sw_ver_list, next) {
		/*
		 * Check if name and version are identical
		 */
		if (!strncmp(img->id.name, swver->name, sizeof(img->id.name)) &&
		    !strncmp(img->id.version, swver->version, sizeof(img->id.version))) {
			TRACE("%s(%s) already installed, skipping...",
				img->id.name,
				img->id.version);
			return true;
		}
	}

	return false;
}

/*
 * function returns:
 * 0 = do not skip the file, it must be installed
 * 1 = skip the file
 * 2 = install directly (stream to the handler)
 * -1= error found
 */
int check_if_required(struct imglist *list, struct filehdr *pfdh,
				struct swver *sw_ver_list,
				struct img_type **pimg)
{
	int skip = SKIP_FILE;
	struct img_type *img;
	int img_skip = 0;
	const char* TMPDIR = get_tmpdir();

	/*
	 * Check that not more as one image wnat to be streamed
	 */
	int install_direct = 0;

	LIST_FOREACH(img, list, next) {
		if (strcmp(pfdh->filename, img->fname) == 0) {

			/*
			 * Check the version. If this artifact is
			 * installed in the same version on the system,
			 * skip it
			 */
			if (isImageInstalled(sw_ver_list, img)) {
				/*
				 *  drop this from the list of images to be installed
				 */
				LIST_REMOVE(img, next);
				img_skip++;
				continue;
			}

			skip = COPY_FILE;
			img->provided = 1;
			img->size = (unsigned int)pfdh->size;

			if (snprintf(img->extract_file,
				     sizeof(img->extract_file), "%s%s",
				     TMPDIR, pfdh->filename) >= (int)sizeof(img->extract_file)) {
				ERROR("Path too long: %s%s", TMPDIR, pfdh->filename);
				return -EBADF;
			}
			/*
			 *  Streaming is possible to only one handler
			 *  If more img requires the same file,
			 *  sw-description contains an error
			 */
			if (install_direct) {
				ERROR("sw-description: stream to several handlers unsupported\n");
				return -EINVAL;
			}

			if (img->install_directly) {
				skip = INSTALL_FROM_STREAM;
				install_direct++;
			}

			*pimg = img;
		}
	}

	if (img_skip > 0)
		skip = SKIP_FILE;

	return skip;
}


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
		extract_next_file(fd, fdout, script->offset, 0,
					script->is_encrypted, script->sha256);
		close(fdout);
	}
	return 0;
}

static int prepare_boot_script(struct swupdate_cfg *cfg, const char *script)
{
	int fd;
	int ret = 0;
	struct dict_entry *bootvar;
	char buf[MAX_BOOT_SCRIPT_LINE_LENGTH];

	fd = openfileoutput(script);
	if (fd < 0)
		return -1;

	LIST_FOREACH(bootvar, &cfg->bootloader, next) {
		if (!bootvar->varname || !bootvar->value)
			continue;
		snprintf(buf, sizeof(buf), "%s %s\n",
			bootvar->varname,
			bootvar->value);
		if (write(fd, buf, strlen(buf)) != (ssize_t)strlen(buf)) {
			  TRACE("Error saving temporary file");
			  ret = -1;
			  break;
		}
	}
	close(fd);
	return ret;
}

static int update_bootloader_env(void)
{
	int ret = 0;

	TRACE("Updating bootloader environment");
	char* bootscript = alloca(strlen(get_tmpdir())+strlen(BOOT_SCRIPT_SUFFIX)+1);
	sprintf(bootscript, "%s%s", get_tmpdir(), BOOT_SCRIPT_SUFFIX);
	ret = bootloader_apply_list(bootscript);
	if (ret < 0)
		ERROR("Error updating bootloader environment");

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

	swupdate_progress_inc_step(img->fname);

	/* TODO : check callback to push results / progress */
	ret = hnd->installer(img, hnd->data);
	if (ret != 0) {
		TRACE("Installer for %s not successful !",
			hnd->desc);
	}

	swupdate_progress_step_completed();

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
	const char* TMPDIR = get_tmpdir();

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
	char* bootscript = alloca(strlen(TMPDIR)+strlen(BOOT_SCRIPT_SUFFIX)+1);
	sprintf(bootscript, "%s%s", TMPDIR, BOOT_SCRIPT_SUFFIX);
	ret = prepare_boot_script(sw, bootscript);
	if (ret) {
		return ret;
	}

	LIST_FOREACH(img, &sw->images, next) {

		/*
		 *  If image is flagged to be installed from stream
		 *  it  was already installed by loading the
		 *  .swu image and it is skipped here.
		 *  This does not make sense when installed from file,
		 *  because images are seekd (no streaming)
		 */
		if (!fromfile && img->install_directly)
			continue;

		if (!fromfile) {
			if (snprintf(filename, sizeof(filename), "%s%s",
				     TMPDIR, img->fname) >= (int)sizeof(filename)) {
				ERROR("Path too long: %s%s", TMPDIR, img->fname);
				return -1;
			}

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

			/*
			 * Skip if the image in the same version is already
			 * installed
			 */
			if (isImageInstalled(&sw->installed_sw_list, img))
				continue;
		}

		if (strncmp(img->path, img->extract_file, sizeof(img->path)) == 0) {
			struct img_type *tmpimg;
			WARN("Temporary and final location for %s is identical, skip "
			     "processing.", img->path);
			LIST_REMOVE(img, next);
			LIST_FOREACH(tmpimg, &sw->images, next) {
				if (strncmp(tmpimg->fname, img->fname, sizeof(img->fname)) == 0) {
					WARN("%s will be removed, it's referenced more "
					     "than once.", img->path);
					break;
				}
			}
			free(img);
			ret = 0;
		} else {
			ret = install_single_image(img);
		}

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

	if (!LIST_EMPTY(&sw->bootloader))
		ret = update_bootloader_env();

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
	struct dict_entry *bootvar;
	struct hw_type *hw;
	const char* TMPDIR = get_tmpdir();

	LIST_FOREACH(img, &software->images, next) {
		if (img->fname[0]) {
			if (snprintf(fn, sizeof(fn), "%s%s", TMPDIR,
				     img->fname) >= (int)sizeof(fn)) {
				ERROR("Path too long: %s%s", TMPDIR, img->fname);
			}
			remove_sw_file(fn);
		}
		LIST_REMOVE(img, next);
		free(img);
	}
	LIST_FOREACH(img, &software->scripts, next) {
		if (img->fname[0]) {
			if (snprintf(fn, sizeof(fn), "%s%s", TMPDIR,
				     img->fname) >= (int)sizeof(fn)) {
				ERROR("Path too long: %s%s", TMPDIR, img->fname);
			}
			remove_sw_file(fn);
		}
		LIST_REMOVE(img, next);
		free(img);
	}
	LIST_FOREACH(bootvar, &software->bootloader, next) {
		dict_remove_entry(bootvar);
	}

	LIST_FOREACH(hw, &software->hardware, next) {
		LIST_REMOVE(hw, next);
		free(hw);
	}
	snprintf(fn, sizeof(fn), "%s%s", TMPDIR, SW_DESCRIPTION_FILENAME);
	remove_sw_file(fn);
#ifdef CONFIG_SIGNED_IMAGES
	snprintf(fn, sizeof(fn), "%s%s.sig",
		TMPDIR, SW_DESCRIPTION_FILENAME);
	remove_sw_file(fn);
#endif
}

int postupdate(struct swupdate_cfg *swcfg, const char *info)
{
	swupdate_progress_done(info);

	if ((swcfg) && (swcfg->globals.postupdatecmd) &&
	    (strnlen(swcfg->globals.postupdatecmd,
		     SWUPDATE_GENERAL_STRING_SIZE) > 0)) {
		DEBUG("Executing post-update command '%s'",
		      swcfg->globals.postupdatecmd);
		int ret = system(swcfg->globals.postupdatecmd);
		if (WIFEXITED(ret)) {
			DEBUG("Post-update command returned %d", WEXITSTATUS(ret));
		} else {
			ERROR("Post-update command returned %d: '%s'", ret, strerror(errno));
			return -1;
		}
	}

	return 0;
}
