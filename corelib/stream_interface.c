/*
 * (C) Copyright 2008-2013
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
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
#include "flash.h"
#include "parsers.h"
#include "network_ipc.h"
#include "network_interface.h"
#include "mongoose_interface.h"
#include "installer.h"

#define BUFF_SIZE	 4096
#define PERCENT_LB_INDEX	4

enum {
	STREAM_WAIT_DESCRIPTION,
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

enum {
	COPY_FILE,
	SKIP_FILE,
	INSTALL_FROM_STREAM
};

/*
 * function returns:
 * 0 = do not skip the file, it must be installed
 * 1 = skip the file
 * 2 = install directly (stream to the handler)
 * -1= error found
 */
static int check_if_required(struct imglist *list, struct filehdr *pfdh,
				struct img_type **pimg)
{
	int skip = SKIP_FILE;
	struct img_type *img;

	/*
	 * Check that not more as one image wnat to be streamed
	 */
	int install_direct = 0;

	LIST_FOREACH(img, list, next) {
		if (strcmp(pfdh->filename, img->fname) == 0) {
			skip = COPY_FILE;
			img->provided = 1;
			img->size = (unsigned int)pfdh->size;

			snprintf(img->extract_file, sizeof(img->extract_file),
					"%s%s", TMPDIR, pfdh->filename);
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

	return skip;
}

static int extract_files(int fd, struct swupdate_cfg *software)
{
	int status = STREAM_WAIT_DESCRIPTION;
	unsigned long offset;
	struct filehdr fdh;
	int skip;
	uint32_t checksum;
	int fdout;
	struct img_type *img;
	char output_file[MAX_IMAGE_FNAME];


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
			if (extract_cpio_header(fd, &fdh, &offset)) {
				return -1;
			}
			if (strcmp(fdh.filename, SW_DESCRIPTION_FILENAME)) {
				TRACE("description file name not the first of the list: %s instead of %s",
					fdh.filename,
					SW_DESCRIPTION_FILENAME);
				return -1;
			}
			snprintf(output_file, sizeof(output_file), "%s%s", TMPDIR, fdh.filename);
			TRACE("Found file:\n\tfilename %s\n\tsize %u", fdh.filename, (unsigned int)fdh.size);

			fdout = openfileoutput(output_file);
			if (fdout < 0)
				return -1;

			if (copyfile(fd, fdout, fdh.size, &offset, 0, 0, &checksum) < 0) {
				return -1;
			}
			if (checksum != (uint32_t)fdh.chksum) {
				ERROR("Checksum WRONG ! Computed 0x%ux, it should be 0x%ux\n",
					(unsigned int)checksum, (unsigned int)fdh.chksum);
				return -1;
			}
			close(fdout);

			if (parse(software, output_file)) {
				ERROR("Compatible SW not found");
				return -1;
			}
			if (check_hw_compatibility(software)) {
				ERROR("SW not compatible with hardware\n");
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

			skip = check_if_required(&software->images, &fdh, &img);
			if (skip == SKIP_FILE) {
				skip = check_if_required(&software->scripts, &fdh, &img);
			}
			TRACE("Found file:\n\tfilename %s\n\tsize %d %s",
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
			case SKIP_FILE:
				if (copyfile(fd, fdout, fdh.size, &offset, skip, 0, &checksum) < 0) {
					return -1;
				}
				close(fdout);
				if (checksum != (unsigned long)fdh.chksum) {
					ERROR("Checksum WRONG ! Computed 0x%ux, it should be 0x%ux",
						(unsigned int)checksum, (unsigned int)fdh.chksum);
					return -1;
				}
				break;
			case INSTALL_FROM_STREAM:
				TRACE("Installing STREAM %s, %lld bytes\n", img->fname, img->size);
				img->fdin = fd;
				if (install_single_image(img)) {
					ERROR("Error streaming %s", img->fname);
					return -1;
				}
				TRACE("END INSTALLIN STREAMING");
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

void *network_initializer(void *data)
{
	int ret;
	pthread_mutex_t condmutex = PTHREAD_MUTEX_INITIALIZER;
	struct swupdate_cfg *software = data;

	/* No installation in progress */
	memset(&inst, 0, sizeof(inst));
	inst.fd = -1;
	inst.status = IDLE;

	/* fork off the local dialogs and network service */
	network_thread_id = start_thread(network_thread, &inst);

	/* handle installation requests (from either source) */
	while (1) {

		printf ("Main loop Daemon\n");

		/* wait for someone to issue an install request */
		pthread_cond_wait(&stream_wkup, &condmutex);
		inst.status = RUN;
		notify(START, RECOVERY_NO_ERROR, "Software Update started !");

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
			fw_set_one_env("recovery_status", "in_progress");

			notify(RUN, RECOVERY_NO_ERROR, "Installation in progress");
			ret = install_images(software, 0, 0);
			if (ret != 0) {
				fw_set_one_env("recovery_status", "failed");
				notify(FAILURE, RECOVERY_ERROR, "Installation failed !");
				inst.last_install = FAILURE;
			} else {
				/*
				 * Clear the recovery variable to indicate to U-Boot
				 * that it is not required to start recovery again
				 */
				fw_set_one_env("recovery_status", "");
				notify(SUCCESS, RECOVERY_NO_ERROR, "SWUPDATE successful !");
				inst.last_install = SUCCESS;
			}
		} else {
			inst.last_install = FAILURE;
			notify(FAILURE, RECOVERY_ERROR, "Image invalid or corrupted. Not installing ...");
		}
		pthread_mutex_lock(&stream_mutex);
		inst.status = IDLE;
		pthread_mutex_unlock(&stream_mutex);
		TRACE("Main thread sleep again !");
		notify(IDLE, RECOVERY_NO_ERROR, "Waiting for requests...");

		/* release temp files we may have created */
		cleanup_files(software);
	}

	pthread_exit((void *)0);
}
