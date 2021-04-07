/*
 * SPDX-FileCopyrightText: 2020 Bosch Sicherheitssysteme GmbH
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#ifdef __FreeBSD__
#include <sys/disk.h>
// the ioctls are almost identical except for the name, just alias it
#define BLKGETSIZE64 DIOCGMEDIASIZE
#else
#include <linux/fs.h>
#endif

#include "swupdate.h"
#include "handler.h"
#include "sslapi.h"
#include "util.h"

void readback_handler(void);
static int readback_postinst(struct img_type *img);

static int readback(struct img_type *img, void *data)
{
	if (!data)
		return -1;

	script_fn scriptfn = *(script_fn *)data;
	switch (scriptfn) {
	case POSTINSTALL:
		return readback_postinst(img);
	case PREINSTALL:
	default:
		return 0;
	}
}

static int readback_postinst(struct img_type *img)
{
	/* Get property: partition hash */
	unsigned char hash[SHA256_HASH_LENGTH];
	char *ascii_hash = dict_get_value(&img->properties, "sha256");
	if (!ascii_hash || ascii_to_hash(hash, ascii_hash) < 0 || !IsValidHash(hash)) {
		ERROR("Invalid hash");
		return -EINVAL;
	}

	/* Get property: partition size */
	unsigned int size = 0;
	char *value = dict_get_value(&img->properties, "size");
	if (value) {
		size = strtoul(value, NULL, 10);
	} else {
		TRACE("Property size not found, use partition size");
	}

	/* Get property: offset */
	unsigned long offset = 0;
	value = dict_get_value(&img->properties, "offset");
	if (value) {
		offset = strtoul(value, NULL, 10);
	} else {
		TRACE("Property offset not found, use default 0");
	}

	/* Open the device (partition) */
	int fdin = open(img->device, O_RDONLY);
	if (fdin < 0) {
		ERROR("Failed to open %s: %s", img->device, strerror(errno));
		return -ENODEV;
	}

	/* Get the real size of the partition, if size is not set. */
	if (size == 0) {
		if (ioctl(fdin, BLKGETSIZE64, &size) < 0) {
			ERROR("Cannot get size of %s", img->device);
			close(fdin);
			return -EFAULT;
		}
		TRACE("Partition size: %u", size);
	}

	/* 
	 * Seek the file descriptor before passing it to copyfile().
	 * This is necessary because copyfile() only accepts streams,
	 * so the file descriptor shall be already at the right position.
	 */
	if (lseek(fdin, offset, SEEK_SET) < 0) {
		ERROR("Seek %lu bytes failed: %s", offset, strerror(errno));
		close(fdin);
		return -EFAULT;
	}

	/*
	 * Perform hash verification. We do not need to pass an output device to
	 * the copyfile() function, because we only want it to verify the hash of
	 * the input device.
	 */
	unsigned long offset_out = 0;
	int status = copyfile(fdin,
			NULL,  /* no output */
			size,
			&offset_out,
			0,     /* no output seek */
			1,     /* skip file, do not write to the output */
			0,     /* no compressed */
			NULL,  /* no checksum */
			hash,
			false,     /* no encrypted */
			NULL,     /* no IVT */
			NULL); /* no callback */
	if (status == 0) {
		INFO("Readback verification success");
	} else {
		ERROR("Readback verification failed, status=%d", status);
	}

	close(fdin);
	return status;
}

__attribute__((constructor))
void readback_handler(void)
{
	register_handler("readback", readback, SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}
