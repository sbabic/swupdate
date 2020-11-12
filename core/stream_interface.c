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
#include "installer_priv.h"
#include "progress.h"
#include "pctl.h"
#include "state.h"

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
	TRACE("Found file");
	TRACE("\tfilename %s", fdh.filename);
	TRACE("\tsize %u", (unsigned int)fdh.size);

	fdout = openfileoutput(output_file);
	if (fdout < 0)
		return -1;

	if (copyfile(fd, &fdout, fdh.size, poffs, 0, 0, 0, &checksum, NULL, 0, NULL, NULL) < 0) {
		close(fdout);
		return -1;
	}
	if (!swupdate_verify_chksum(checksum, fdh.chksum)) {
		close(fdout);
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
	bool installed_directly = false;

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
			if (preupdatecmd(software)) {
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
 				/*
			 	 * Keep reading the cpio padding, if any, up
				 * to 512 bytes from the socket until the
				 * client stops writing
			 	 */
				extract_padding(fd, &offset);
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

			TRACE("Found file");
			TRACE("\tfilename %s", fdh.filename);
			TRACE("\tsize %u %s", (unsigned int)fdh.size,
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
				if (copyfile(fd, &fdout, fdh.size, &offset, 0, 0, 0, &checksum, img->sha256, 0, NULL, NULL) < 0) {
					close(fdout);
					return -1;
				}
				if (!swupdate_verify_chksum(checksum, fdh.chksum)) {
					close(fdout);
					return -1;
				}
				close(fdout);
				break;

			case SKIP_FILE:
				if (copyfile(fd, &fdout, fdh.size, &offset, 0, skip, 0, &checksum, NULL, 0, NULL, NULL) < 0) {
					return -1;
				}
				if (!swupdate_verify_chksum(checksum, fdh.chksum)) {
					return -1;
				}
				break;
			case INSTALL_FROM_STREAM:
				TRACE("Installing STREAM %s, %lld bytes", img->fname, img->size);

				/*
				 * If this is the first image to be directly installed, set transaction flag
				 * to on to be checked if a power-off happens. Be sure to set the flag
				 * just once
				 */
				if (!installed_directly) {
					if (software->bootloader_transaction_marker) {
						save_state_string((char*)BOOTVAR_TRANSACTION, STATE_IN_PROGRESS);
					}
					installed_directly = true;
				}

				/*
				 * If we are streaming data to store in a UBI volume, make
				 * sure that the UBI partitions are adjusted beforehand
				 */
				LIST_FOREACH(part, &software->images, next) {
					if (!part->install_directly && part->is_partitioner) {
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
				if (  img->skip)
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

static int cpfiles(int fdin, int fdout, size_t max)
{
	char *buf;
	const int bufsize = 16 * 1024;
	int ret, len;
	size_t maxread;
	bool cpyall = (max == 0);

	buf = (char *)malloc(bufsize);
	if (!buf)
		return -ENOMEM;

	for (;;) {
		/*
		 * No limit set - copy the whole file
		 * Set max to be always not zero
		 */
		if (cpyall)
			max =  2 * bufsize;
		maxread = min(bufsize, max);
		len = read(fdin, buf, maxread);
		if (len < 0) {
			free(buf);
			return -EIO;
		}
		if (len == 0)
			break;
		ret = copy_write(&fdout, buf, len);
		if (ret < 0) {
			free(buf);
			return -EIO;
		}
		max -= len;
		if (max == 0)
			break;
	}

	free(buf);
	return 0;
}


#define SW_TMP_OUTPUT	"swtmp-outputXXXXXXXX"
static int save_stream(int fdin, struct swupdate_cfg *software)
{
	unsigned char *buf;
	int fdout = -1, ret, len;
	const int bufsize = 16 * 1024;
	int tmpfd = -1;
	char tmpfilename[MAX_IMAGE_FNAME];
	struct filehdr fdh;
	unsigned int tmpsize;
	unsigned long offset;
	char output_file[MAX_IMAGE_FNAME];
	const char* TMPDIR = get_tmpdir();

	if (fdin < 0)
		return -EINVAL;

	snprintf(tmpfilename, sizeof(tmpfilename), "%s/%s", TMPDIR, SW_TMP_OUTPUT);

	buf = (unsigned char *)malloc(bufsize);
	if (!buf) {
		ERROR("OOM when saving stream");
		return -ENOMEM;
	}

	/*
	 * Cache the beginning of the SWU to parse
	 * sw-description and check if the output must be
	 * redirected. This allows to define the output file on demand
	 * setting it into sw-description.
	 */
	tmpfd = mkstemp(tmpfilename);
	if (tmpfd < 0) {
		ERROR("Cannot get space for temporary data, error %d", errno);
		ret = -EFAULT;
		goto no_copy_output;
	}
	len = read(fdin, buf, bufsize);
	if (len < 0) {
		ERROR("Reading from file failed, error %d", errno);
		ret = -EFAULT;
		goto no_copy_output;
	}
	if (get_cpiohdr(buf, &fdh.size, &fdh.namesize, &fdh.chksum) < 0) {
		ERROR("CPIO Header corrupted, cannot be parsed");
		ret = -EINVAL;
		goto no_copy_output;
	}

	/*
	 * Make an estimation for sw-description and signature.
	 * Signature cannot be very big - if it is, it is an attack.
	 * So let a buffer just for the signature - tmpsize is enough for both
	 * sw-description and sw-description.sig, if any.
	 */
	tmpsize = SWUPDATE_ALIGN(fdh.size + fdh.namesize + sizeof(struct new_ascii_header) + bufsize - len,
			bufsize);
	ret = copy_write(&tmpfd, buf, len);  /* copy the first buffer */
	if (ret < 0) {
		ret =  -EIO;
		goto no_copy_output;
	}

	/*
	 * copy enough bytes to have sw-description and sw-description.sig
	 */
	ret = cpfiles(fdin, tmpfd, tmpsize);
	if (ret < 0) {
		ret =  -EIO;
		goto no_copy_output;
	}
	lseek(tmpfd, 0, SEEK_SET);
	offset = 0;

	if (extract_file_to_tmp(tmpfd, SW_DESCRIPTION_FILENAME, &offset) < 0) {
		ERROR("%s cannot be extracted", SW_DESCRIPTION_FILENAME);
		ret = -EINVAL;
		goto no_copy_output;
	}
#ifdef CONFIG_SIGNED_IMAGES
	snprintf(output_file, sizeof(output_file), "%s.sig", SW_DESCRIPTION_FILENAME);
	if (extract_file_to_tmp(tmpfd, output_file, &offset) < 0 ) {
		ERROR("Signature cannot be extracted:%s", output_file);
		ret = -EINVAL;
		goto no_copy_output;
	}

#endif
	snprintf(output_file, sizeof(output_file), "%s%s", TMPDIR, SW_DESCRIPTION_FILENAME);
	if (parse(software, output_file)) {
		ERROR("Compatible SW not found");
		ret = -1;
		goto no_copy_output;
	}

	/*
	 * if all is ok, copy the first part of SWU (stored in tmp file)
	 * into the output
	 */
	lseek(tmpfd, 0, SEEK_SET);

	fdout = openfileoutput(software->output);
	if (fdout < 0)
		return -1;

	ret = cpfiles(tmpfd, fdout, 0);
	if (ret < 0)
		goto no_copy_output;

	ret = cpfiles(fdin, fdout, 0);
	if (ret < 0)
		goto no_copy_output;

	ret = 0;

no_copy_output:
	free(buf);
	if (fdout >= 0)
		close(fdout);
	if (tmpfd >= 0) {
		close(tmpfd);
		unlink(tmpfilename);
	}

	cleanup_files(software);

	return ret;
}

void *network_initializer(void *data)
{
	int ret;
	struct swupdate_cfg *software = data;
	struct swupdate_request *req;

	/* No installation in progress */
	memset(&inst, 0, sizeof(inst));
	inst.fd = -1;
	inst.status = IDLE;
	inst.software = software;

	/* fork off the local dialogs and network service */
	network_thread_id = start_thread(network_thread, &inst);

	/* handle installation requests (from either source) */
	while (1) {
		ret = 0;
		TRACE("Main loop Daemon");

		/* wait for someone to issue an install request */
		pthread_mutex_lock(&stream_mutex);
		pthread_cond_wait(&stream_wkup, &stream_mutex);
		inst.status = RUN;
		pthread_mutex_unlock(&stream_mutex);
		notify(START, RECOVERY_NO_ERROR, INFOLEVEL, "Software Update started !");

		req = &inst.req;

		/*
		 * Check if the dry run flag is overwritten
		 */
		if (req->dry_run)
			software->globals.dry_run = 1;
		else
			software->globals.dry_run = 0;

		/*
		 * Find the selection to be installed
		 */
		if ((strnlen(req->software_set, sizeof(req->software_set)) > 0) &&
				(strnlen(req->running_mode, sizeof(req->running_mode)) > 0)) {
			strlcpy(software->software_set, req->software_set, sizeof(software->software_set) - 1);
			strlcpy(software->running_mode, req->running_mode, sizeof(software->running_mode) - 1);
		} else {
			strlcpy(software->software_set, software->globals.default_software_set,
				sizeof(software->software_set) - 1);
			strlcpy(software->running_mode, software->globals.default_running_mode,
				sizeof(software->running_mode) - 1);
		}

		/*
		 * Check if the stream should be saved
		 */
		if (strlen(software->output)) {
			ret = save_stream(inst.fd, software);
			if (ret < 0) {
				notify(FAILURE, RECOVERY_ERROR, ERRORLEVEL,
					"Error saving stream, not installing ...");
			}

			/*
			 * now replace the file descriptor with
			 * the saved file
			 */
			if (!(inst.fd < 0))
				close(inst.fd);
			inst.fd = open(software->output, O_RDONLY,  S_IRUSR);
		}

		if (!ret) {
#ifdef CONFIG_MTD
			mtd_cleanup();
			scan_mtd_devices();
#endif
			/*
		 	 * extract the meta data and relevant parts
		 	 * (flash images) from the install image
		 	 */
			ret = extract_files(inst.fd, software);
		}
		if (!(inst.fd < 0))
			close(inst.fd);

		/* do carry out the installation (flash programming) */
		if (ret == 0) {
			TRACE("Valid image found: copying to FLASH");

			/*
			 * If an image is loaded, the install
			 * must be successful. Set we have
			 * initiated an update
			 */
			if (software->bootloader_transaction_marker) {
				save_state_string((char*)BOOTVAR_TRANSACTION, STATE_IN_PROGRESS);
			}

			notify(RUN, RECOVERY_NO_ERROR, INFOLEVEL, "Installation in progress");
			ret = install_images(software, 0, 0);
			if (ret != 0) {
				if (software->bootloader_transaction_marker) {
					save_state_string((char*)BOOTVAR_TRANSACTION, STATE_FAILED);
				}
				notify(FAILURE, RECOVERY_ERROR, ERRORLEVEL, "Installation failed !");
				inst.last_install = FAILURE;
				if (save_state((char *)STATE_KEY, STATE_FAILED) != SERVER_OK) {
					WARN("Cannot persistently store FAILED update state.");
				}
			} else {
				/*
				 * Clear the recovery variable to indicate to bootloader
				 * that it is not required to start recovery again
				 */
				if (software->bootloader_transaction_marker) {
					unset_state((char*)BOOTVAR_TRANSACTION);
				}
				if (save_state((char *)STATE_KEY, STATE_INSTALLED) != SERVER_OK) {
					ERROR("Cannot persistently store INSTALLED update state.");
					notify(FAILURE, RECOVERY_ERROR, ERRORLEVEL, "Installation failed !");
					inst.last_install = FAILURE;
				} else {
					notify(SUCCESS, RECOVERY_NO_ERROR, INFOLEVEL, "SWUPDATE successful !");
					inst.last_install = SUCCESS;
				}
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
	len = min(len - 1, strlen(inst.req.info));
	strncpy(buf, inst.req.info, len);
	*source = inst.req.source;

	return len;
}
