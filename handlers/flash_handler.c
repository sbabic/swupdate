/*
 * (C) Copyright 2008
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * See file CREDITS for list of people who contributed to this
 * project.
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

#include <sys/types.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <sys/ioctl.h>

#include <mtd/mtd-user.h>
#include "swupdate.h"
#include "handler.h"
#include "util.h"

#define PROCMTD	"/proc/mtd"
#define BUFF_SIZE	8192
#define LINESIZE	80

void flash_handler(void);

int flash_get_mtd_number(char *partition_name, char *mtd_device, unsigned int len)
{
	FILE *fp;
	char line[LINESIZE];
	unsigned int i, baselen, found = 0;

	fp = fopen(PROCMTD, "r");
	if (fp == NULL) {
		TRACE("Impossible to open %s", PROCMTD);
		return -1;
	}
	strncpy(mtd_device, "/dev/", len);
	baselen = strlen(mtd_device);
	while (fgets(line, LINESIZE, fp) != NULL && !found) {
		if (!strstr(line, partition_name))
			continue;
		for (i = 0; i < strlen(line); i++) {
			/* returns if the buffer is too small */
			if (i == len) 
				return -EINVAL;
			if (line[i] != ':')
				mtd_device[i + baselen] = line[i];
			else {
				mtd_device[i + baselen] = '\0';
				break;
			}
		}
		found = 1;
	}
	fclose(fp);
	return (-(!found));
}

static int flash_partition_erase(char *partition, char *filename)
{
	struct stat sb;
	off_t erasetotalsize;
	int fd;
	char mtd_device[LINESIZE];
	mtd_info_t meminfo;
	struct mtd_dev_info mtd;
	erase_info_t erase;

	if (flash_get_mtd_number(partition, mtd_device, LINESIZE))
		return -1;

	if ((fd = open(mtd_device, O_RDWR)) < 0) {
		TRACE( "%s: %s: %s", __func__, mtd_device, strerror(errno));
		return -1;
	}

	if (ioctl(fd, MEMGETINFO, &meminfo) != 0) {
		TRACE( "%s: %s: unable to get MTD device info", __func__, mtd_device);
		close(fd);
		return -1;
	}

	/*
	 * prepare to erase all of the MTD partition,
	 * limit the erase operation to the file's size when known,
	 * don't start erasing if the file exceeds the partition
	 */
	erase.length = meminfo.erasesize;
	erasetotalsize = meminfo.size;
	if ((stat(filename, &sb) == 0) && (S_ISREG(sb.st_mode))) {
		if ((unsigned int)sb.st_size > meminfo.size) {
			TRACE( "%s: %s: image file larger than partition", __func__, mtd_device);
			close(fd);
			return -1;
		}
		erasetotalsize = sb.st_size;
		erasetotalsize += meminfo.erasesize - 1;
		erasetotalsize /= meminfo.erasesize;
		erasetotalsize *= meminfo.erasesize;
	}

	for (erase.start = 0; erase.start < (unsigned int)erasetotalsize; erase.start += meminfo.erasesize) {
		if (ioctl(fd, MEMERASE, &erase) != 0) {
			TRACE( "\n%s: %s: MTD Erase failure: %s", __func__, mtd_device, strerror(errno));
			continue;
		}
	}
	close(fd);

	return 0;
}

static int flash_install_image(struct img_type *img)
{
	int fdout;
	char mtd_device[LINESIZE];
	char *buf;
	int ret;
	uint32_t checksum;
	long unsigned int dummy = 0;

	if (flash_get_mtd_number(img->device, mtd_device, LINESIZE))
		return -1;

	if ((fdout = open(mtd_device, O_RDWR)) < 0) {
		TRACE( "%s: %s: %s", __func__, mtd_device, strerror(errno));
		return -1;
	}

	buf = (char *)malloc(BUFF_SIZE);
	if (!buf) {
		TRACE("malloc returns no memory");
		return -ENOMEM;
	}

	ret = copyfile(img->fdin, fdout, img->size, &dummy, 0, 0, &checksum);
	free(buf);

	/* tell 'nbytes == 0' (EOF) from 'nbytes < 0' (read error) */
	if (ret < 0) {
		TRACE("Failure installing into: %s", img->device);
		return -1;
	}
	close(fdout);
	return 0;
}

static int install_flash_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	char filename[64];
	struct flash_description *flash = get_flash_info();

	snprintf(filename, sizeof(filename), "%s%s", TMPDIR, img->fname);

	if(flash_partition_erase(img->device,
		filename)) {
		TRACE("I cannot erasing %s partition",
			img->device);
		return -1;
	}
	TRACE("Copying %s", img->fname);
	if (flash_install_image(img)) {
		TRACE("I cannot copy %s into %s partition",
			img->fname,
			img->device);
		return -1;
	}

	return 0;
}

__attribute__((constructor))
void flash_handler(void)
{
	register_handler("flash", install_flash_image, NULL);
}
