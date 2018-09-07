/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
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
#include <sys/wait.h>
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

/*
 * function returns:
 * 0 = do not skip the file, it must be installed
 * 1 = skip the file
 * 2 = install directly (stream to the handler)
 * -1= error found
 */
int check_if_required(struct imglist *list, struct filehdr *pfdh,
				const char *destdir,
				struct img_type **pimg)
{
	int skip = SKIP_FILE;
	struct img_type *img;

	/*
	 * Check that not more than one image want to be streamed
	 */
	int install_direct = 0;

	LIST_FOREACH(img, list, next) {
		if (strcmp(pfdh->filename, img->fname) == 0) {
			skip = COPY_FILE;
			img->provided = 1;
			img->size = (unsigned int)pfdh->size;

			if (snprintf(img->extract_file,
				     sizeof(img->extract_file), "%s%s",
				     destdir, pfdh->filename) >= (int)sizeof(img->extract_file)) {
				ERROR("Path too long: %s%s", destdir, pfdh->filename);
				return -EBADF;
			}
			/*
			 *  Streaming is possible to only one handler
			 *  If more img requires the same file,
			 *  sw-description contains an error
			 */
			if (install_direct) {
				ERROR("sw-description: stream to several handlers unsupported");
				return -EINVAL;
			}

			if (img->install_directly) {
				skip = INSTALL_FROM_STREAM;
				install_direct++;
			}

			*pimg = img;
		}
	}

	return skip;
}


/*
 * Extract all scripts from a list from the image
 * and save them on the filesystem to be executed later
 */
static int extract_scripts(int fd, struct imglist *head, int fromfile)
{
	struct img_type *script;
	int fdout;
	int ret = 0;
	const char* tmpdir_scripts = get_tmpdirscripts();

	LIST_FOREACH(script, head, next) {
		if (script->provided == 0) {
			ERROR("Required script %s not found in image",
				script->fname);
			return -1;
		}

		snprintf(script->extract_file, sizeof(script->extract_file), "%s%s",
			 tmpdir_scripts , script->fname);

		fdout = openfileoutput(script->extract_file);
		if (fdout < 0)
			return fdout;

		if (fromfile)
			ret = extract_next_file(fd, fdout, script->offset, 0,
						script->is_encrypted, script->sha256);
		else {
			int fdin;
			char *tmpfile;
			unsigned long offset = 0;
			uint32_t checksum;

			if (asprintf(&tmpfile, "%s%s", get_tmpdir(), script->fname) ==
				ENOMEM_ASPRINTF) {
				ERROR("Path too long: %s%s", get_tmpdir(), script->fname);
				close(fdout);
				return -ENOMEM;
			}

			fdin = open(tmpfile, O_RDONLY);
			free(tmpfile);
			if (fdin < 0) {
				ERROR("Extracted script not found in %s: %s %d",
					get_tmpdir(), script->extract_file, errno);
				return -ENOENT;
			}

			ret = copyfile(fdin, &fdout, script->size, &offset, 0, 0,
					script->compressed,
					&checksum,
					script->sha256,
					script->is_encrypted,
					NULL);
			close(fdin);
		}
		close(fdout);

		if (ret < 0)
			return ret;
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
		char *key = dict_entry_get_key(bootvar);
		char *value = dict_entry_get_value(bootvar);

		if (!key || !value)
			continue;
		snprintf(buf, sizeof(buf), "%s %s\n", key, value);
		if (write(fd, buf, strlen(buf)) != (ssize_t)strlen(buf)) {
			  TRACE("Error saving temporary file");
			  ret = -1;
			  break;
		}
	}
	close(fd);
	return ret;
}

static int run_prepost_scripts(struct imglist *list, script_fn type)
{
	int ret;
	struct img_type *img;
	struct installer_handler *hnd;

	/* Scripts must be run before installing images */
	LIST_FOREACH(img, list, next) {
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

int install_single_image(struct img_type *img, int dry_run)
{
	struct installer_handler *hnd;
	int ret;

	/*
	 * in case of dry run, replace the handler
	 * with a dummy doing nothing
	 */
	if (dry_run)
		strcpy(img->type, "dummy");

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
	char *filename;
	struct filehdr fdh;
	struct stat buf;
	const char* TMPDIR = get_tmpdir();
	int dry_run = sw->globals.dry_run;

	/* Extract all scripts, preinstall scripts must be run now */
	const char* tmpdir_scripts = get_tmpdirscripts();
	ret = extract_scripts(fdsw, &sw->scripts, fromfile);
	ret |= extract_scripts(fdsw, &sw->bootscripts, fromfile);
	if (ret) {
		ERROR("extracting script to %s failed", tmpdir_scripts);
		return ret;
	}

	/* Scripts must be run before installing images */
	if (!dry_run) {
		ret = run_prepost_scripts(&sw->scripts, PREINSTALL);
		if (ret) {
			ERROR("execute preinstall scripts failed");
			return ret;
		}
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
		    if (asprintf(&filename, "%s%s", TMPDIR, img->fname) ==
				ENOMEM_ASPRINTF) {
				ERROR("Path too long: %s%s", TMPDIR, img->fname);
				return -1;
			}

			ret = stat(filename, &buf);
			if (ret) {
				TRACE("%s not found or wrong", filename);
				free(filename);
				return -1;
			}
			img->size = buf.st_size;

			img->fdin = open(filename, O_RDONLY);
			free(filename);
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

		if ((strlen(img->path) > 0) &&
			(strlen(img->extract_file) > 0) &&
			(strncmp(img->path, img->extract_file, sizeof(img->path)) == 0)){
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
			free_image(img);
			ret = 0;
		} else {
			ret = install_single_image(img, dry_run);
		}

		if (!fromfile)
			close(img->fdin);

		if (ret)
			return ret;

	}

	/*
	 * Skip scripts in dry-run mode
	 */
	if (dry_run) {
		return ret;
	}

	ret = run_prepost_scripts(&sw->scripts, POSTINSTALL);
	if (ret) {
		ERROR("execute postinstall scripts failed");
		return ret;
	}

	if (!LIST_EMPTY(&sw->bootloader))
		ret = update_bootloader_env();

	ret |= run_prepost_scripts(&sw->bootscripts, POSTINSTALL);

	return ret;
}

static void remove_sw_file(char __attribute__ ((__unused__)) *fname)
{
#ifndef CONFIG_NOCLEANUP
	/* yes, "best effort", the files need not necessarily exist */
	unlink(fname);
#endif
}

static void cleaup_img_entry(struct img_type *img)
{
	char *fn;
	const char *tmp[] = { get_tmpdirscripts(), get_tmpdir() };

	if (img->fname[0]) {
		for (unsigned int i = 0; i < ARRAY_SIZE(tmp); i++) {
			if (asprintf(&fn, "%s%s", tmp[i], img->fname) == ENOMEM_ASPRINTF) {
				ERROR("Path too long: %s%s", tmp[i], img->fname);
			} else {
				remove_sw_file(fn);
				free(fn);
			}
		}
	}
}

void free_image(struct img_type *img) {
	dict_drop_db(&img->properties);
	free(img);
}

void cleanup_files(struct swupdate_cfg *software) {
	char fn[64];
	struct img_type *img;
	struct img_type *img_tmp;
	struct hw_type *hw;
	struct hw_type *hw_tmp;
	const char* TMPDIR = get_tmpdir();
	struct imglist *list[] = {&software->scripts, &software->bootscripts};

	LIST_FOREACH_SAFE(img, &software->images, next, img_tmp) {
		if (img->fname[0]) {
			if (snprintf(fn, sizeof(fn), "%s%s", TMPDIR,
				     img->fname) >= (int)sizeof(fn)) {
				ERROR("Path too long: %s%s", TMPDIR, img->fname);
			}
			remove_sw_file(fn);
		}
		LIST_REMOVE(img, next);
		free_image(img);
	}

	for (unsigned int count = 0; count < ARRAY_SIZE(list); count++) {
		LIST_FOREACH_SAFE(img, list[count], next, img_tmp) {
			cleaup_img_entry(img);

			LIST_REMOVE(img, next);
			free_image(img);
		}
	}

	dict_drop_db(&software->bootloader);

	snprintf(fn, sizeof(fn), "%s%s", TMPDIR, BOOT_SCRIPT_SUFFIX);
	remove_sw_file(fn);

	LIST_FOREACH_SAFE(hw, &software->hardware, next, hw_tmp) {
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
