/*
 * (C) Copyright 2008-2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <pthread.h>
#include "cpiohdr.h"

#include "bsdqueue.h"
#include "swupdate.h"
#include "util.h"
#include "handler.h"
#ifdef CONFIG_MTD
#include "flash.h"
#endif
#include "parsers.h"
#include "network_ipc.h"
#include "network_interface.h"
#include "mongoose_interface.h"
#include "installer.h"
#include "progress.h"
#include "pctl.h"
#include "bootloader.h"

#define BUFF_SIZE	 4096
#define PERCENT_LB_INDEX	4

enum {
	STREAM_WAIT_DESCRIPTION,
	STREAM_WAIT_SIGNATURE,
	STREAM_DATA,
	STREAM_END
};

static pthread_t network_thread_id;

/*
 * NOTE: these sync vars are _not_ static, they are _shared_ between the
 * installer, the display thread and the network thread
 *
 * 'mutex' protects the 'inst' installer data, the 'cond' variable as
 * well as the 'inst.mnu_main' close request; 'cond' signals the
 * reception of an install request
 *
 */

pthread_mutex_t stream_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t stream_wkup = PTHREAD_COND_INITIALIZER;

static struct installer inst;

static int extract_file_to_tmp(int fd, const char *fname, unsigned long *poffs)
{
	char output_file[MAX_IMAGE_FNAME];
	struct filehdr fdh;
	int fdout;
	uint32_t checksum;
	const char* TMPDIR = get_tmpdir();

	if (extract_cpio_header(fd, &fdh, poffs)) {
		return -1;
	}
	if (strcmp(fdh.filename, fname)) {
		TRACE("description file name not the first of the list: %s instead of %s",
			fdh.filename,
			fname);
		return -1;
	}
	if (snprintf(output_file, sizeof(output_file), "%s%s", TMPDIR,
		     fdh.filename) >= (int)sizeof(output_file)) {
		ERROR("Path too long: %s%s", TMPDIR, fdh.filename);
		return -1;
	}
	TRACE("Found file:\n\tfilename %s\n\tsize %u", fdh.filename, (unsigned int)fdh.size);

	fdout = openfileoutput(output_file);
	if (fdout < 0)
		return -1;

	if (copyfile(fd, &fdout, fdh.size, poffs, 0, 0, 0, &checksum, NULL, 0, NULL) < 0) {
		close(fdout);
		return -1;
	}
	if (checksum != (uint32_t)fdh.chksum) {
		close(fdout);
		ERROR("Checksum WRONG ! Computed 0x%ux, it should be 0x%ux",
			(unsigned int)checksum, (unsigned int)fdh.chksum);
			return -1;
	}
	close(fdout);

	return 0;
}

static int extract_files(int fd, struct swupdate_cfg *software)
{
	int status = STREAM_WAIT_DESCRIPTION;
	unsigned long offset;
	struct filehdr fdh;
	int skip;
	uint32_t checksum;
	int fdout;
	struct img_type *img, *part;
	char output_file[MAX_IMAGE_FNAME];
	const char* TMPDIR = get_tmpdir();

	/* preset the info about the install parts */

	offset = 0;

#ifdef CONFIG_UBIVOL
	mtd_init();
	ubi_init();
#endif

	for (;;) {
		switch (status) {
		/* Waiting for the first Header */
		case STREAM_WAIT_DESCRIPTION:
			if (extract_file_to_tmp(fd, SW_DESCRIPTION_FILENAME, &offset) < 0 )
				return -1;

			status = STREAM_WAIT_SIGNATURE;
			break;

		case STREAM_WAIT_SIGNATURE:
#ifdef CONFIG_SIGNED_IMAGES
			snprintf(output_file, sizeof(output_file), "%s.sig", SW_DESCRIPTION_FILENAME);
			if (extract_file_to_tmp(fd, output_file, &offset) < 0 )
				return -1;
#endif
			snprintf(output_file, sizeof(output_file), "%s%s", TMPDIR, SW_DESCRIPTION_FILENAME);
			if (parse(software, output_file)) {
				ERROR("Compatible SW not found");
				return -1;
			}

			if (check_hw_compatibility(software)) {
				ERROR("SW not compatible with hardware");
				return -1;
			}
			status = STREAM_DATA;
			break;

		case STREAM_DATA:
			if (extract_cpio_header(fd, &fdh, &offset)) {
				ERROR("CPIO HEADER");
				return -1;
			}
			if (strcmp("TRAILER!!!", fdh.filename) == 0) {
				status = STREAM_END;
				break;
			}

			struct imglist *list[] = {&software->images,
						  &software->scripts,
						  &software->bootscripts};

			for (unsigned int i = 0; i < ARRAY_SIZE(list); i++) {
				skip = check_if_required(list[i], &fdh,
						get_tmpdir(),
						&img);

				if (skip != SKIP_FILE)
					break;
			}

			TRACE("Found file:\n\tfilename %s\n\tsize %u %s",
				fdh.filename,
				(unsigned int)fdh.size,
				(skip == SKIP_FILE ? "Not required: skipping" : "required"));

			fdout = -1;
			offset = 0;

			/*
			 * If images are not streamed directly into the target
			 * copy them into TMPDIR to check if it is all ok
			 */
			switch (skip) {
			case COPY_FILE:
				fdout = openfileoutput(img->extract_file);
				if (fdout < 0)
					return -1;
				if (copyfile(fd, &fdout, fdh.size, &offset, 0, 0, 0, &checksum, img->sha256, 0, NULL) < 0) {
					close(fdout);
					return -1;
				}
				if (checksum != (unsigned long)fdh.chksum) {
					ERROR("Checksum WRONG ! Computed 0x%ux, it should be 0x%ux",
						(unsigned int)checksum, (unsigned int)fdh.chksum);
					close(fdout);
					return -1;
				}
				close(fdout);
				break;

			case SKIP_FILE:
				if (copyfile(fd, &fdout, fdh.size, &offset, 0, skip, 0, &checksum, NULL, 0, NULL) < 0) {
					return -1;
				}
				if (checksum != (unsigned long)fdh.chksum) {
					ERROR("Checksum WRONG ! Computed 0x%ux, it should be 0x%ux",
						(unsigned int)checksum, (unsigned int)fdh.chksum);
					return -1;
				}
				break;
			case INSTALL_FROM_STREAM:
				TRACE("Installing STREAM %s, %lld bytes", img->fname, img->size);
				/*
				 * If we are streaming data to store in a UBI volume, make
				 * sure that the UBI partitions are adjusted beforehand
				 */
				LIST_FOREACH(part, &software->images, next) {
					if ( (!part->install_directly)
						&& (!strcmp(part->type, "ubipartition")) ) {
						TRACE("Need to adjust partition %s before streaming %s",
							part->volname, img->fname);
						if (install_single_image(part, software->globals.dry_run)) {
							ERROR("Error adjusting partition %s", part->volname);
							return -1;
						}
						/* Avoid trying to adjust again later */
						part->install_directly = 1;
					}
				}
				img->fdin = fd;
				if (install_single_image(img, software->globals.dry_run)) {
					ERROR("Error streaming %s", img->fname);
					return -1;
				}
				TRACE("END INSTALLING STREAMING");
				break;
			}

			break;

		case STREAM_END:

			/*
			 * Check if all required files were provided
			 * Update of a single file is not possible.
			 */

			LIST_FOREACH(img, &software->images, next) {
				if (! img->required)
					continue;
				if (! img->fname[0])
					continue;
				if (! img->provided) {
					ERROR("Required image file %s missing...aborting !",
						img->fname);
					return -1;
				}
			}
			return 0;
		default:
			return -1;
		}
	}
}

static int save_stream(int fdin, const char *output)
{
	char *buf;
	int fdout, ret, len;
	const int bufsize = 16 * 1024;

	fdout = openfileoutput(output);
	if (fdout < 0)
		return -1;

	buf = (char *)malloc(bufsize);
	if (!buf)
		return -ENOMEM;

	for (;;) {
		len = read(fdin, buf, bufsize);
		if (len == 0)
			break;
		ret = copy_write(&fdout, buf, len);
		if (ret < 0)
			return -EIO;
	}

	return 0;
}

void *network_initializer(void *data)
{
	int ret;
	struct swupdate_cfg *software = data;

	/* No installation in progress */
	memset(&inst, 0, sizeof(inst));
	inst.fd = -1;
	inst.status = IDLE;

	/* fork off the local dialogs and network service */
	network_thread_id = start_thread(network_thread, &inst);

	/* handle installation requests (from either source) */
	while (1) {

		TRACE("Main loop Daemon");

		/* wait for someone to issue an install request */
		pthread_mutex_lock(&stream_mutex);
		pthread_cond_wait(&stream_wkup, &stream_mutex);
		inst.status = RUN;
		pthread_mutex_unlock(&stream_mutex);
		notify(START, RECOVERY_NO_ERROR, INFOLEVEL, "Software Update started !");

		/*
		 * Check if the dryrun flag is overwrittn
		 */
		if (inst.dry_run)
			software->globals.dry_run = 1;

		/*
		 * Check if the stream should be saved
		 */
		if (strlen(software->output)) {
			ret = save_stream(inst.fd, software->output);
			if (ret < 0) {
				notify(FAILURE, RECOVERY_ERROR, ERRORLEVEL,
					"Image invalid or corrupted. Not installing ...");
				continue;
			}

			/*
			 * now replace the file descriptor with
			 * the saved file
			 */
			close(inst.fd);
			inst.fd = open(software->output, O_RDONLY,  S_IRUSR);
		}


#ifdef CONFIG_MTD
		mtd_cleanup();
		scan_mtd_devices();
#endif
		/*
		 * extract the meta data and relevant parts
		 * (flash images) from the install image
		 */
		ret = extract_files(inst.fd, software);
		close(inst.fd);

		/* do carry out the installation (flash programming) */
		if (ret == 0) {
			TRACE("Valid image found: copying to FLASH");

			/*
			 * If an image is loaded, the install
			 * must be successful. Set we have
			 * initiated an update
			 */
			bootloader_env_set("recovery_status", "in_progress");

			notify(RUN, RECOVERY_NO_ERROR, INFOLEVEL, "Installation in progress");
			ret = install_images(software, 0, 0);
			if (ret != 0) {
				bootloader_env_set("recovery_status", "failed");
				notify(FAILURE, RECOVERY_ERROR, ERRORLEVEL, "Installation failed !");
				inst.last_install = FAILURE;

			} else {
				/*
				 * Clear the recovery variable to indicate to bootloader
				 * that it is not required to start recovery again
				 */
				bootloader_env_unset("recovery_status");
				notify(SUCCESS, RECOVERY_NO_ERROR, INFOLEVEL, "SWUPDATE successful !");
				inst.last_install = SUCCESS;
			}
		} else {
			inst.last_install = FAILURE;
			notify(FAILURE, RECOVERY_ERROR, ERRORLEVEL, "Image invalid or corrupted. Not installing ...");
		}

		swupdate_progress_end(inst.last_install);

		pthread_mutex_lock(&stream_mutex);
		inst.status = IDLE;
		pthread_mutex_unlock(&stream_mutex);
		TRACE("Main thread sleep again !");
		notify(IDLE, RECOVERY_NO_ERROR, INFOLEVEL, "Waiting for requests...");

		/* release temp files we may have created */
		cleanup_files(software);
	}

	pthread_exit((void *)0);
}

/*
 * Retrieve additional info sent by the source
 * The data is not locked because it is retrieve
 * at different times
 */
int get_install_info(sourcetype *source, char *buf, size_t len)
{
	len = min(len, inst.len);

	memcpy(buf, inst.info, len);
	*source = inst.source;

	return len;
}

