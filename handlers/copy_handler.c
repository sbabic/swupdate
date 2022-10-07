/*
 * (C) Copyright 2022
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
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
#include <mtd/mtd-user.h>
#ifdef __FreeBSD__
#include <sys/disk.h>
// the ioctls are almost identical except for the name, just alias it
#define BLKGETSIZE64 DIOCGMEDIASIZE
#else
#include <linux/fs.h>
#endif

#include <pctl.h>
#include "swupdate.h"
#include "handler.h"
#include "util.h"
#include "chained_handler.h"
#include "installer.h"

#define PIPE_READ  0
#define PIPE_WRITE 1

static void copy_handler(void);
static void raw_copyimage_handler(void);

static int copy_image_file(struct img_type *img, void *data)
{
	int ret;
	int fdout, fdin;
	struct dict_list *proplist;
	struct dict_list_elem *entry;
	uint32_t checksum;
	unsigned long offset = 0;
	size_t size;
	struct stat statbuf;
	struct script_handler_data *script_data;
	struct chain_handler_data priv;
	pthread_t chain_handler_thread_id;
	int pipes[2];
	char *path = NULL;
	struct mtd_info_user	mtdinfo;

	if (!data)
		return -1;

	script_data = data;

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

	path = realpath(entry->value, NULL);
	if (!path) {
		ERROR("%s cannot be resolved", entry->value);
		return -EINVAL;
	}

	/*
	 * Get information about source (size)
	 */
	fdin = open(path, O_RDONLY);
	if (fdin < 0) {
		ERROR("%s cannot be opened: %s", path, strerror(errno));
		free(path);
		return -EINVAL;
	}

	ret = fstat(fdin, &statbuf);
	if (ret < 0) {
		ERROR("Cannot be retrieved information on %s", path);
		free(path);
		close(fdin);
		return -ENODEV;
	}

	/*
	 * Detect the size if not set in sw-descriptiont
	 */
	size = dict_get_value(&img->properties, "size") ? ustrtoull(dict_get_value(&img->properties, "size"), NULL, 0) : 0;
	if (!size) {
		if (S_ISREG(statbuf.st_mode))
			size = statbuf.st_size;
		else if (S_ISBLK(statbuf.st_mode) && (ioctl(fdin, BLKGETSIZE64, &size) < 0)) {
			ERROR("Cannot get size of Block Device %s", path);
		} else if (S_ISCHR(statbuf.st_mode)) {
			/* it is maybe a MTD device, just try */
			ret = ioctl(fdin, MEMGETINFO, &mtdinfo);
			if (!ret)
				size = mtdinfo.size;
		}
	}

	if (!size) {
		ERROR("Size cannot be detected for %s", path);
		free(path);
		close(fdin);
		return -ENODEV;
	}

	TRACE("Copying %lu from %s to %s", size, path, img->device);

	free(path);

	if (!size) {
		ERROR("Size cannot be detected, please set it, exiting...");
		close(fdin);
		return -EFAULT;
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

	/*
	 * No chain set, fallback to rawcopy
	 */
	proplist = dict_get_list(&img->properties, "chain");
	if (!proplist || !(entry = LIST_FIRST(proplist))) {
		WARN("No chained handler set, fallback to rawcopy");
		strncpy(priv.img.type, "raw", sizeof(priv.img.type));
	} else {
		strlcpy(priv.img.type, entry->value, sizeof(priv.img.type));
		TRACE("Set %s handler in the chain", priv.img.type);
	}

	fdout = pipes[PIPE_WRITE];
	signal(SIGPIPE, SIG_IGN);

	chain_handler_thread_id = start_thread(chain_handler_thread, &priv);
	wait_threads_ready();

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

	DEBUG("Chained handler returned %d", ret);

	close(fdin);
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
