/*
 * (C) Copyright 2022
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <libgen.h>
#include <mtd/mtd-user.h>
#include <ftw.h>
#ifdef __FreeBSD__
#include <sys/disk.h>
// the ioctls are almost identical except for the name, just alias it
#define BLKGETSIZE64 DIOCGMEDIASIZE
#else
#include <linux/fs.h>
#endif

#include <pctl.h>
#include "swupdate_image.h"
#include "progress.h"
#include "handler.h"
#include "util.h"
#include "chained_handler.h"
#include "installer.h"

#define PIPE_READ  0
#define PIPE_WRITE 1

static void copy_handler(void);
static void raw_copyimage_handler(void);

char *copyfrom;
struct img_type *base_img;
char *chained_handler;

static int copy_single_file(const char *path, ssize_t size, struct img_type *img, const char *chained)
{
	int fdout, fdin, ret;
	struct stat statbuf;
	struct mtd_info_user	mtdinfo;
	struct chain_handler_data priv;
	uint32_t checksum;
	int pipes[2];
	unsigned long offset = 0;
	pthread_t chain_handler_thread_id;

	/*
	 * Get information about source (size)
	 */
	fdin = open(path, O_RDONLY);
	if (fdin < 0) {
		ERROR("%s cannot be opened: %s", path, strerror(errno));
		return -EINVAL;
	}

	ret = fstat(fdin, &statbuf);
	if (ret < 0) {
		ERROR("Cannot be retrieved information on %s", path);
		close(fdin);
		return -ENODEV;
	}

	/*
	 * try to detect the size if was not set in sw-description
	 */
	if (!size) {
		if (S_ISREG(statbuf.st_mode)) {
			size = statbuf.st_size;
		}
		else if (S_ISBLK(statbuf.st_mode) && (ioctl(fdin, BLKGETSIZE64, &size) < 0)) {
			ERROR("Cannot get size of Block Device %s", path);
			size = -1;
		} else if (S_ISCHR(statbuf.st_mode)) {
			/* it is maybe a MTD device, just try */
			ret = ioctl(fdin, MEMGETINFO, &mtdinfo);
			if (!ret) {
				size = mtdinfo.size;
			} else {
				size = -1;
			}
		}
	}

	if (size < 0) {
		ERROR("Size cannot be detected for %s", path);
		close(fdin);
		return -ENODEV;
	}

	if (pipe(pipes) < 0) {
		ERROR("Could not create pipes for chained handler, existing...");
		close(fdin);
		return -EFAULT;
	}

	/* Overwrite some parameters for chained handler */
	memcpy(&priv.img, img, sizeof(*img));
	priv.img.compressed = COMPRESSED_FALSE;
	memset(priv.img.sha256, 0, SHA256_HASH_LENGTH);
	priv.img.fdin = pipes[PIPE_READ];
	priv.img.size = size;
	strlcpy(priv.img.type, chained, sizeof(priv.img.type));

	fdout = pipes[PIPE_WRITE];
	signal(SIGPIPE, SIG_IGN);

	chain_handler_thread_id = start_thread(chain_handler_thread, &priv);
	wait_threads_ready();

	/*
	 * Copying from device itself,
	 * no encryption or compression
	 */
	ret = copyfile(fdin,
			&fdout,
			size,
			&offset,
			0,
			0, /* no skip */
			0, /* no compressed */
			&checksum,
			0, /* no sha256 */
			false, /* no encrypted */
			NULL, /* no IVT */
			NULL);

	close(fdout);
	void *status;
	ret = pthread_join(chain_handler_thread_id, &status);
	if (ret) {
		ERROR("return code from pthread_join() is %d", ret);
	} else
		ret = (unsigned long)status;

	close(fdin);

	return ret;

}

static int recurse_directory(const char *fpath, const struct stat *sb,
			     int typeflag, __attribute__ ((__unused__)) struct FTW *ftwbuf)
{
	const char *relpath = "";
	char *dst;
	struct stat statbufdst;
	struct img_type cpyimg;
	int ret, result = FTW_CONTINUE;

	if (strstr(fpath, copyfrom) != fpath)
		return result;

	relpath = fpath + strlen(copyfrom);

	if (!strlen(relpath))
		return FTW_CONTINUE;

	dst = malloc(strlen(base_img->path) + strlen(relpath) + 1);
	strcpy(dst, base_img->path);
	strcat(dst, relpath);

	switch (typeflag) {
	case FTW_D:
		errno = 0;
		ret = stat(dst, &statbufdst);
		if (ret < 0 && errno == ENOENT) {
			ret = mkdir(dst, sb->st_mode);
			if (ret) {
				free(dst);
				return ret;
			}
		}
		break;
	case FTW_F:
		memcpy(&cpyimg, base_img, sizeof(cpyimg));
		strlcpy(cpyimg.path, dst, sizeof(cpyimg.path));

		/*
		 * Note: copying a directory is counted just once as step
		 * before an update is started. But then a a handler
		 * is called for each found file, and this increases the number
		 * of steps. So increase it before copying.
		 */
		swupdate_progress_addstep();
		if (copy_single_file(fpath, 0, &cpyimg, chained_handler))
			result = FTW_STOP;
	}

	free(dst);

	return result;
}

static int copy_image_file(struct img_type *img, void *data)
{
	int ret = 0;
	struct dict_list *proplist;
	struct dict_list_elem *entry;
	size_t size;
	struct script_handler_data *script_data;
	bool recursive, createdest;

	if (!data)
		return -1;

	script_data = data;

	base_img = img;

	proplist = dict_get_list(&img->properties, "type");
	if (proplist) {
		entry = LIST_FIRST(proplist);
		/* check if this should just run as pre or post install */
		if (entry) {
			if (strcmp(entry->value, "preinstall") && strcmp(entry->value, "postinstall")) {
				ERROR("Type can be just preinstall or postinstall");
				return -EINVAL;
			}
			script_fn type = !strcmp(entry->value, "preinstall") ? PREINSTALL : POSTINSTALL;
			if (type != script_data->scriptfn) {
				TRACE("Script set to %s, skipping", entry->value);
				return 0;
			}
		}
	}

	proplist = dict_get_list(&img->properties, "copyfrom");
	if (!proplist || !(entry = LIST_FIRST(proplist))) {
		ERROR("Missing source device, no copyfrom property");
		return -EINVAL;
	}

	copyfrom = realpath(entry->value, NULL);
	if (!copyfrom) {
		ERROR("%s cannot be resolved", entry->value);
		return -EINVAL;
	}

	/*
	 * Detect the size if not set in sw-descriptiont
	 */
	size = dict_get_value(&img->properties, "size") ? ustrtoull(dict_get_value(&img->properties, "size"), NULL, 0) : 0;

	/*
	 * No chain set, fallback to rawcopy
	 */
	proplist = dict_get_list(&img->properties, "chain");
	if (!proplist || !(entry = LIST_FIRST(proplist))) {
		WARN("No chained handler set, fallback to rawcopy");
		chained_handler = strdup("raw");
	} else {
		chained_handler = strndup(entry->value, sizeof(img->type) - 1);
		TRACE("Set %s handler in the chain", chained_handler);
	}

	/*
	 * Check for recursive copy
	 */
	TRACE("Copying %zu from %s to %s", size, copyfrom, img->device);

	recursive = strtobool(dict_get_value(&img->properties, "recursive"));
	createdest = strtobool(dict_get_value(&img->properties, "create-destination"));


	if (createdest) {
		ret = mkpath(recursive ? base_img->path : dirname(base_img->path), 0755);
		if (ret < 0) {
			ERROR("I cannot create path %s: %s",
				recursive ? base_img->path : dirname(base_img->path),
				strerror(errno));
			ret = -EFAULT;
		}
	}

	if (!ret) {
		if (recursive) {
			ret = nftw(copyfrom, recurse_directory, 64, FTW_PHYS);

		} else {
			swupdate_progress_addstep();
			ret = copy_single_file(copyfrom, size, img, chained_handler);
		}
	}

	free(copyfrom);
	free(chained_handler);

	return ret;
}

__attribute__((constructor))
void copy_handler(void)
{
	register_handler("copy", copy_image_file,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}

__attribute__((constructor))
void raw_copyimage_handler(void)
{
	register_handler("rawcopy", copy_image_file,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}
