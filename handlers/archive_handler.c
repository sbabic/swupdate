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
	int exitval;
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
	struct archive *ext = NULL;
	struct archive_entry *entry = NULL;
	int r;
	int flags;
	struct extract_data *data = (struct extract_data *)p;
	flags = data->flags;
	int exitval = -EFAULT;

	a = archive_read_new();
	if (!a) {
		goto out;
	}

	ext = archive_write_disk_new();
	if (!ext) {
		goto out;
	}

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
		goto out;
	}
	for (;;) {
		r = archive_read_next_header(a, &entry);
		if (r == ARCHIVE_EOF)
			break;
		if (r != ARCHIVE_OK) {
			ERROR("archive_read_next_header(): %s %d",
			    archive_error_string(a), 1);
			goto out;
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
				goto out;
			}
		}

	}

	exitval = 0;

out:
	if (ext) {
		r = archive_write_free(ext);
		if (r) {
			ERROR("archive_write_free(): %s %d",
					archive_error_string(a), r);
			exitval = -EFAULT;
		}
	}

	if (a) {
		archive_read_close(a);
		archive_read_free(a);
	}

	data->exitval = exitval;
	pthread_exit(NULL);
}

static int install_archive_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	char path[255];
	int fdout = -1;
	int ret = -1;
	int thread_ret = -1;
	char pwd[256] = "\0";
	struct extract_data tf;
	pthread_attr_t attr;
	int use_mount = (strlen(img->device) && strlen(img->filesystem)) ? 1 : 0;
	int is_mounted = 0;
	int exitval = -EFAULT;

	char* DATADST_DIR = alloca(strlen(get_tmpdir())+strlen(DATADST_DIR_SUFFIX)+1);
	sprintf(DATADST_DIR, "%s%s", get_tmpdir(), DATADST_DIR_SUFFIX);

	char * FIFO = alloca(strlen(get_tmpdir())+strlen(FIFO_FILE_NAME)+1);
	sprintf(FIFO, "%s%s", get_tmpdir(), FIFO_FILE_NAME);

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

		is_mounted = 1;

		if (snprintf(path, sizeof(path), "%s%s",
			DATADST_DIR, img->path) >= (int)sizeof(path)) {
			ERROR("Path too long: %s%s", DATADST_DIR, img->path);
			goto out;
		}
	} else {
		if (snprintf(path, sizeof(path), "%s", img->path) >= (int)sizeof(path)) {
			ERROR("Path too long: %s", img->path);
			goto out;
		}
	}

	unlink(FIFO);
	ret = mkfifo(FIFO, 0600);
	if (ret) {
		ERROR("FIFO cannot be created in archive handler");
		goto out;
	}

	if (!getcwd(pwd, sizeof(pwd))) {
		ERROR("Failed to determine current working directory");
		pwd[0] = '\0';
		goto out;
	}

	/*
	 * Change to directory where tarball must be extracted
	 */
	ret = chdir(path);
	if (ret) {
		ERROR("Fault: chdir not possible");
		goto out;
	}

	TRACE("Installing file %s on %s, %s attributes",
		img->fname, path,
		img->preserve_attributes ? "preserving" : "ignoring");

	tf.flags = 0;
	tf.exitval = -EFAULT;

	if (img->preserve_attributes) {
		tf.flags |= ARCHIVE_EXTRACT_OWNER | ARCHIVE_EXTRACT_PERM |
				ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_ACL |
				ARCHIVE_EXTRACT_FFLAGS | ARCHIVE_EXTRACT_XATTR;
	}

	thread_ret = pthread_create(&extract_thread, &attr, extract, &tf);
	if (thread_ret) {
		ERROR("Code from pthread_create() is %d",
			 thread_ret);
		goto out;
	}

	fdout = open(FIFO, O_WRONLY);
	if (fdout < 0) {
		ERROR("Failed to open FIFO %s", FIFO);
		goto out;
	}

	ret = copyimage(&fdout, img, NULL);
	if (ret < 0) {
		ERROR("Error copying extracted file");
		goto out;
	}

	exitval = 0;

out:
	if (fdout >= 0) {
		ret = close(fdout);
		if (ret) {
			ERROR("failed to close FIFO %s", FIFO);
		}
	}

	if (!thread_ret) {
		void *status;

		ret = pthread_join(extract_thread, &status);
		if (ret) {
			ERROR("return code from pthread_join() is %d", ret);
			exitval = -EFAULT;
		}
		else if (tf.exitval != 0) {
			ERROR("copyimage status code is %d", tf.exitval);
			exitval = -EFAULT;
		}
	}

	if (pwd[0]) {
		ret = chdir(pwd);
		if (ret) {
			ERROR("chdir failed to revert to directory %s", pwd);
		}
	}

	unlink(FIFO);

	if (is_mounted) {
		ret = swupdate_umount(DATADST_DIR);
		if (ret) {
			TRACE("Failed to unmount directory %s", DATADST_DIR);
		}
	}

	return exitval;
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
