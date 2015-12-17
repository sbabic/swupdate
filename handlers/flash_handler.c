/*
 * (C) Copyright 2014
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
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
#include <stdbool.h>
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
#include "flash.h"

#define PROCMTD	"/proc/mtd"
#define LINESIZE	80

void flash_handler(void);

/*
 * Note: the functions here are derived directly
 * with minor changes from mtd-utils.
 */

static int flash_erase(int mtdnum)
{
	int fd;
	char mtd_device[LINESIZE];
	struct mtd_dev_info *mtd;
	int noskipbad = 0;
	int unlock = 0;
	unsigned int eb, eb_start, eb_cnt;
	struct flash_description *flash = get_flash_info();

	if  (!mtd_dev_present(flash->libmtd, mtdnum)) {
			ERROR("MTD %d does not exist\n", mtdnum);
			return -ENODEV;
	}
	mtd = &flash->mtd_info[mtdnum].mtd;
	snprintf(mtd_device, sizeof(mtd_device), "/dev/mtd%d", mtdnum);

	if ((fd = open(mtd_device, O_RDWR)) < 0) {
		ERROR( "%s: %s: %s", __func__, mtd_device, strerror(errno));
		return -ENODEV;
	}

	/*
	 * prepare to erase all of the MTD partition,
	 */
	eb_start = 0;
	eb_cnt = (mtd->size / mtd->eb_size) - eb_start;
	for (eb = 0; eb < eb_start + eb_cnt; eb++) {

		/* Always skip bad sectors */
		if (!noskipbad) {
			int ret = mtd_is_bad(mtd, fd, eb);
			if (ret > 0) {
				continue;
			} else if (ret < 0) {
				if (errno == EOPNOTSUPP) {
					noskipbad = 1;
				} else {
					ERROR("%s: MTD get bad block failed", mtd_device);
					return -EFAULT;
				}
			}
		}

		if (unlock) {
			if (mtd_unlock(mtd, fd, eb) != 0) {
				TRACE("%s: MTD unlock failure", mtd_device);
				continue;
			}
		}

		if (mtd_erase(flash->libmtd, mtd, fd, eb) != 0) {
			ERROR("%s: MTD Erase failure", mtd_device);
			return -EFAULT;
		}
	}

	close(fd);

	return 0;
}

/*
 * Writing to the NAND must take into account ECC errors
 * and BAD sectors.
 * This is not required for NOR flashes
 * The function reassembles nandwrite from mtd-utils
 * dropping all options that are not required here.
 */

static void erase_buffer(void *buffer, size_t size)
{
	const uint8_t kEraseByte = 0xff;

	if (buffer != NULL && size > 0)
		memset(buffer, kEraseByte, size);
}

static int flash_write_nand(int mtdnum, struct img_type *img)
{
	char mtd_device[LINESIZE];
	struct flash_description *flash = get_flash_info();
	struct mtd_dev_info *mtd = &flash->mtd_info[mtdnum].mtd;
	int pagelen;
	bool baderaseblock = false;
	long long imglen = 0;
	long long blockstart = -1;
	long long offs;
	unsigned char *filebuf = NULL;
	size_t filebuf_max = 0;
	size_t filebuf_len = 0;
	long long mtdoffset = 0;
	int ifd = img->fdin;
	int fd = -1;
	bool failed = true;
	int ret;
	unsigned char *writebuf = NULL;

	pagelen = mtd->min_io_size;
	imglen = img->size;
	snprintf(mtd_device, sizeof(mtd_device), "/dev/mtd%d", mtdnum);

	if ((imglen / pagelen) * mtd->min_io_size > mtd->size) {
		ERROR("Image %s does not fit into mtd%d\n", img->fname, mtdnum);
		return -EIO;
	}

	/* Flashing to NAND is currently not streamable */
	if (img->install_directly) {
		ERROR("Raw NAND not streamable\n");
		return -EINVAL;
	}

	filebuf_max = mtd->eb_size / mtd->min_io_size * pagelen;
	filebuf = calloc(1, filebuf_max);
	erase_buffer(filebuf, filebuf_max);

	if ((fd = open(mtd_device, O_RDWR)) < 0) {
		ERROR( "%s: %s: %s", __func__, mtd_device, strerror(errno));
		return -ENODEV;
	}

	/*
	 * Get data from input and write to the device while there is
	 * still input to read and we are still within the device
	 * bounds. Note that in the case of standard input, the input
	 * length is simply a quasi-boolean flag whose values are page
	 * length or zero.
	 */
	while ((imglen > 0 || writebuf < filebuf + filebuf_len)
		&& mtdoffset < mtd->size) {
		/*
		 * New eraseblock, check for bad block(s)
		 * Stay in the loop to be sure that, if mtdoffset changes because
		 * of a bad block, the next block that will be written to
		 * is also checked. Thus, we avoid errors if the block(s) after the
		 * skipped block(s) is also bad
		 */
		while (blockstart != (mtdoffset & (~mtd->eb_size + 1))) {
			blockstart = mtdoffset & (~mtd->eb_size + 1);
			offs = blockstart;

			/*
			 * if writebuf == filebuf, we are rewinding so we must
			 * not reset the buffer but just replay it
			 */
			if (writebuf != filebuf) {
				erase_buffer(filebuf, filebuf_len);
				filebuf_len = 0;
				writebuf = filebuf;
			}

			baderaseblock = false;

			do {
				ret = mtd_is_bad(mtd, fd, offs / mtd->eb_size);
				if (ret < 0) {
					ERROR("mtd%d: MTD get bad block failed", mtdnum);
					goto closeall;
				} else if (ret == 1) {
					baderaseblock = true;
				}

				if (baderaseblock) {
					mtdoffset = blockstart + mtd->eb_size;

					if (mtdoffset > mtd->size) {
						ERROR("too many bad blocks, cannot complete request");
						goto closeall;
					}
				}

				offs +=  mtd->eb_size; 
			} while (offs < blockstart + mtd->eb_size);
		}

		/* Read more data from the input if there isn't enough in the buffer */
		if (writebuf + mtd->min_io_size > filebuf + filebuf_len) {
			size_t readlen = mtd->min_io_size;
			size_t alreadyread = (filebuf + filebuf_len) - writebuf;
			size_t tinycnt = alreadyread;
			ssize_t cnt = 0;

			while (tinycnt < readlen) {
				cnt = read(ifd, writebuf + tinycnt, readlen - tinycnt);
				if (cnt == 0) { /* EOF */
					break;
				} else if (cnt < 0) {
					ERROR("File I/O error on input");
					goto closeall;
				}
				tinycnt += cnt;
			}

			/* No padding needed - we are done */
			if (tinycnt == 0) {
				imglen = 0;
				break;
			}

			/* Padding */
			if (tinycnt < readlen) {
				erase_buffer(writebuf + tinycnt, readlen - tinycnt);
			}

			filebuf_len += readlen - alreadyread;

			imglen -= tinycnt - alreadyread;

		}

		/* Write out data */
		ret = mtd_write(flash->libmtd, mtd, fd, mtdoffset / mtd->eb_size,
				mtdoffset % mtd->eb_size,
				writebuf,
				mtd->min_io_size,
				NULL,
				0,
				MTD_OPS_PLACE_OOB);
		if (ret) {
			long long i;
			if (errno != EIO) {
				ERROR("mtd%d: MTD write failure", mtdnum);
				goto closeall;
			}

			/* Must rewind to blockstart if we can */
			writebuf = filebuf;

			for (i = blockstart; i < blockstart + mtd->eb_size; i += mtd->eb_size) {
				if (mtd_erase(flash->libmtd, mtd, fd, i / mtd->eb_size)) {
					int errno_tmp = errno;
					TRACE("mtd%d: MTD Erase failure", mtdnum);
					if (errno_tmp != EIO)
						goto closeall;
				}
			}

			TRACE("Marking block at %08llx bad\n",
					mtdoffset & (~mtd->eb_size + 1));
			if (mtd_mark_bad(mtd, fd, mtdoffset / mtd->eb_size)) {
				ERROR("mtd%d: MTD Mark bad block failure", mtdnum);
				goto closeall;
			}
			mtdoffset = blockstart + mtd->eb_size;

			continue;
		}
		mtdoffset += mtd->min_io_size;
		writebuf += pagelen;
	}
	failed = false;

closeall:
	free(filebuf);
	close(fd);

	if (failed) {
		ERROR("Installing image %s into mtd%d failed\n",
			img->fname,
			mtdnum);
		return -1;
	}

	return 0;
}

static int flash_write_nor(int mtdnum, struct img_type *img)
{
	int fdout;
	char mtd_device[LINESIZE];
	int ret;
	uint32_t checksum;
	long unsigned int dummy = 0;
	struct flash_description *flash = get_flash_info();

	if  (!mtd_dev_present(flash->libmtd, mtdnum)) {
		ERROR("MTD %d does not exist\n", mtdnum);
		return -ENODEV;
	}

	snprintf(mtd_device, sizeof(mtd_device), "/dev/mtd%d", mtdnum);
	if ((fdout = open(mtd_device, O_RDWR)) < 0) {
		ERROR( "%s: %s: %s", __func__, mtd_device, strerror(errno));
		return -1;
	}

	ret = copyfile(img->fdin, fdout, img->size, &dummy, 0, 0, &checksum);

	/* tell 'nbytes == 0' (EOF) from 'nbytes < 0' (read error) */
	if (ret < 0) {
		ERROR("Failure installing into: %s", img->device);
		return -1;
	}
	close(fdout);
	return 0;
}

static int flash_write_image(int mtdnum, struct img_type *img)
{
	struct flash_description *flash = get_flash_info();

	if (!isNand(flash, mtdnum))
		return flash_write_nor(mtdnum, img);

	return flash_write_nand(mtdnum, img);
}

static int install_flash_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	char filename[64];
	int mtdnum;

	snprintf(filename, sizeof(filename), "%s%s", TMPDIR, img->fname);
	mtdnum= get_mtd_from_device(img->device);

	if (mtdnum < 0) {
		ERROR("Wrong MTD device in description: %s",
			img->device);
		return -1;
	}

	if(flash_erase(mtdnum)) {
		ERROR("I cannot erasing %s",
			img->device);
		return -1;
	}
	TRACE("Copying %s", img->fname);
	if (flash_write_image(mtdnum, img)) {
		ERROR("I cannot copy %s into %s partition",
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
