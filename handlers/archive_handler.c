/*
 * (C) Copyright 2015
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <sys/types.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

#include <archive.h>
#include <archive_entry.h>

#include "swupdate.h"
#include "handler.h"
#include "util.h"

#define FIFO_FILE_NAME "archivfifo"

/* Just to turn on during development */
static int debug = 0;

void untar_handler(void);
void archive_handler(void);

pthread_t extract_thread;

struct extract_data {
	int flags;
};

static int
copy_data(struct archive *ar, struct archive *aw)
{
	int r;
	const void *buff;
	size_t size;
#if ARCHIVE_VERSION_NUMBER >= 3000000
	int64_t offset;
#else
	off_t offset;
#endif

	for (;;) {
		r = archive_read_data_block(ar, &buff, &size, &offset);
		if (r == ARCHIVE_EOF)
			return (ARCHIVE_OK);
		if (r != ARCHIVE_OK)
			return (r);
		r = archive_write_data_block(aw, buff, size, offset);
		if (r != ARCHIVE_OK) {
			TRACE("archive_write_data_block(): %s",
			    archive_error_string(aw));
			return (r);
		}
	}
}

static void *
extract(void *p)
{
	struct archive *a;
	struct archive *ext;
	struct archive_entry *entry;
	int r;
	int flags;
	struct extract_data *data = (struct extract_data *)p;
	flags = data->flags;

	a = archive_read_new();
	ext = archive_write_disk_new();
	archive_write_disk_set_options(ext, flags);
	/*
	 * Note: archive_write_disk_set_standard_lookup() is useful
	 * here, but it requires library routines that can add 500k or
	 * more to a static executable.
	 */
	archive_read_support_format_all(a);
	archive_read_support_filter_all(a);

	/*
	 * On my system, enabling other archive formats adds 20k-30k
	 * each.  Enabling gzip decompression adds about 20k.
	 * Enabling bzip2 is more expensive because the libbz2 library
	 * isn't very well factored.
	 */
	char* FIFO = alloca(strlen(get_tmpdir())+strlen(FIFO_FILE_NAME)+1);
	sprintf(FIFO, "%s%s", get_tmpdir(), FIFO_FILE_NAME);
	if ((r = archive_read_open_filename(a, FIFO, 4096))) {
		ERROR("archive_read_open_filename(): %s %d",
		    archive_error_string(a), r);
		pthread_exit((void *)-1);
	}
	for (;;) {
		r = archive_read_next_header(a, &entry);
		if (r == ARCHIVE_EOF)
			break;
		if (r != ARCHIVE_OK) {
			ERROR("archive_read_next_header(): %s %d",
			    archive_error_string(a), 1);
			pthread_exit((void *)-1);
		}

		if (debug)
			TRACE("Extracting %s", archive_entry_pathname(entry));

		r = archive_write_header(ext, entry);
		if (r != ARCHIVE_OK)
			TRACE("archive_write_header(): %s",
			    archive_error_string(ext));
		else {
			copy_data(a, ext);
			r = archive_write_finish_entry(ext);
			if (r != ARCHIVE_OK)  {
				ERROR("archive_write_finish_entry(): %s",
				    archive_error_string(ext));
				pthread_exit((void *)-1);
			}
		}

	}

	archive_read_close(a);
	archive_read_free(a);
	pthread_exit((void *)0);
}

static int install_archive_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	char path[255];
	int fdout;
	int ret = 0;
	char pwd[256];
	struct extract_data tf;
	pthread_attr_t attr;
	void *status;
	int use_mount = (strlen(img->device) && strlen(img->filesystem)) ? 1 : 0;

	char* DATADST_DIR = alloca(strlen(get_tmpdir())+strlen(DATADST_DIR_SUFFIX)+1);
	sprintf(DATADST_DIR, "%s%s", get_tmpdir(), DATADST_DIR_SUFFIX);

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	if (strlen(img->path) == 0) {
		TRACE("Missing path attribute");
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

	char* FIFO = alloca(strlen(get_tmpdir())+strlen(FIFO_FILE_NAME)+1);
	sprintf(FIFO, "%s%s", get_tmpdir(), FIFO_FILE_NAME);
	unlink(FIFO);
	ret = mkfifo(FIFO, 0666);
	if (ret) {
		ERROR("FIFO cannot be created in archive handler");
		return -1;
	}
	if (!getcwd(pwd, sizeof(pwd)))
		return -1;

	/*
	 * Change to directory where tarball must be extracted
	 */
	ret = chdir(path);
	if (ret) {
		ERROR("Fault: chdir not possible");
		return -EFAULT;
	}

	TRACE("Installing file %s on %s, %s attributes",
		img->fname, path,
		img->preserve_attributes ? "preserving" : "ignoring");

	tf.flags = 0;

	if (img->preserve_attributes) {
		tf.flags |= ARCHIVE_EXTRACT_OWNER | ARCHIVE_EXTRACT_PERM |
				ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_ACL |
				ARCHIVE_EXTRACT_FFLAGS | ARCHIVE_EXTRACT_XATTR;
	}

	ret = pthread_create(&extract_thread, &attr, extract, &tf);
	if (ret) {
		ERROR("Code from pthread_create() is %d",
			 ret);
		return -EFAULT;
	}

	fdout = open(FIFO, O_WRONLY);

	ret = copyimage(&fdout, img, NULL);
	if (ret< 0) {
		ERROR("Error copying extracted file");
		return -EFAULT;
	}

	close(fdout);

	ret = pthread_join(extract_thread, &status);
	if (ret) {
		ERROR("return code from pthread_join() is %d", ret);
		return -EFAULT;
	}

	unlink(FIFO);

	ret = chdir(pwd);

	if (ret) {
		TRACE("Fault: chdir not possible");
	}

	if (use_mount) {
		swupdate_umount(DATADST_DIR);
	}

	return ret;
}

__attribute__((constructor))
void archive_handler(void)
{
	register_handler("archive", install_archive_image,
				IMAGE_HANDLER | FILE_HANDLER, NULL);
}

/* This is an alias for the parsers */
__attribute__((constructor))
void untar_handler(void)
{
	register_handler("tar", install_archive_image,
				IMAGE_HANDLER | FILE_HANDLER, NULL);
}
