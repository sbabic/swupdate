/*
 * (C) Copyright 2014-2016
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * Hamming code from
 * https://github.com/martinezjavier/writeloader
 * Copyright (C) 2011 ISEE 2007, SL
 * Author: Javier Martinez Canillas <martinez.javier@gmail.com>
 * Author: Agusti Fontquerni Gorchs <afontquerni@iseebcn.com>
 * Overview:
 *   Writes a loader binary to a NAND flash memory device and calculates
 *   1-bit Hamming ECC codes to fill the MTD's out-of-band (oob) area
 *   independently of the ECC technique implemented on the NAND driver.
 *   This is a workaround required for TI ARM OMAP DM3730 ROM boot to load.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <linux/version.h>
#include <sys/ioctl.h>

#include <mtd/mtd-user.h>
#include "handler.h"
#include "swupdate_image.h"
#include "util.h"
#include "flash.h"
#include "progress.h"

#define PROCMTD	"/proc/mtd"
#define LINESIZE	80

static inline int mtd_error(int mtdnum, const char *func, int eb)
{
	int code = errno;
	ERROR("mtd%d: mtd_%s() failed at block %d: %s (errno = %d)", mtdnum,
	      func, eb, STRERROR(code), code);
	return -code;
}
#define MTD_ERROR(func) mtd_error(priv->mtdnum, (func), priv->eb)

#define MTD_TRACE(func, msg) do { \
		int _code = errno; \
		TRACE("mtd%d: mtd_%s() %s at block %d: %s (errno = %d)", \
		      priv->mtdnum, (func), (msg), priv->eb, \
		      STRERROR(_code), _code); \
	} while (0)
#define MTD_TRACE_FAILED(func) MTD_TRACE((func), "failed")
#define MTD_TRACE_NOT_SUPPORTED(func) MTD_TRACE((func), "not supported")

static inline int too_many_bad_blocks(int mtdnum)
{
	ERROR("mtd%d: too many bad blocks", mtdnum);
	return -ENOSPC;
}

void flash_handler(void);

/* Check whether buffer is filled with character 'pattern' */
static inline int buffer_check_pattern(unsigned char *buffer, size_t size,
                                       unsigned char pattern)
{
        /* Invalid input */
        if (!buffer || (size == 0))
                return 0;

        /* No match on first byte */
        if (*buffer != pattern)
                return 0;

        /* First byte matched and buffer is 1 byte long, OK. */
        if (size == 1)
                return 1;

        /*
         * Check buffer longer than 1 byte. We already know that buffer[0]
         * matches the pattern, so the test below only checks whether the
         * buffer[0...size-2] == buffer[1...size-1] , which is a test for
         * whether the buffer is filled with constant value.
         */
        return !memcmp(buffer, buffer + 1, size - 1);
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
	if (buffer != NULL && size > 0)
		memset(buffer, FLASH_EMPTY_BYTE, size);
}

struct flash_priv {
	/* File descriptor of a flash device (/dev/mtdX) to write into: */
	int fdout;
	/* Index of a flash device (X in /dev/mtdX): */
	int mtdnum;
	/* Number of image bytes we still need to write into flash: */
	long long imglen;
	/* A buffer for caching data soon to be written to flash: */
	unsigned char *filebuf;
	/* Amount of bytes we've read from image file into filebuf: */
	int filebuf_len;
	/* An offset within filebuf to write to flash starting from: */
	int writebuf_offset;
	/* Current flash erase block: */
	int eb;
	/*
	 * The following data is kept here only to speed-up execution:
	 */
	int eb_end; /* First invalid erase block (after flash memory end). */
	struct mtd_dev_info *mtd;
	libmtd_t libmtd;
	bool is_nand; /* is it NAND or some other (e.g. NOR) flash type? */
	bool check_bad; /* do we need to check for bad blocks? */
	bool check_locked; /* do we need to check whether a block is locked? */
	bool do_unlock; /* do we need to try to unlock a block? */
	unsigned char *readout_buf; /* a buffer to read erase block into */
};

static int erase_block(struct flash_priv *priv)
{
	int ret;
	ret = mtd_erase(priv->libmtd, priv->mtd, priv->fdout, priv->eb);
	if (ret) {
		if (is_not_supported(errno)) {
			/* Some MTD drivers in linux kernel may not initialize
			 * mtd->erasesize or master->_erase, hence we expect
			 * these errors in such cases. */
			MTD_TRACE_NOT_SUPPORTED("erase");
			return 0;
		} else if (errno == EIO) { /* May happen for a bad block. */
			MTD_TRACE_FAILED("erase");
			return -EIO;
		} else
			return MTD_ERROR("erase");
	}
	return 0;
}

static int mark_bad_block(struct flash_priv *priv)
{
	int ret;
	TRACE("mtd%d: marking block %d (offset %lld) as bad", priv->mtdnum,
	      priv->eb, (long long)(priv->eb) * priv->mtd->eb_size);
	ret = mtd_mark_bad(priv->mtd, priv->fdout, priv->eb);
	if (ret) {
		if (is_not_supported(errno)) {
			/* Some MTD drivers in linux kernel may not initialize
			 * master->_block_markbad, hence we expect these errors
			 * in such cases. */
			MTD_TRACE_NOT_SUPPORTED("mark_bad");
		} else
			return MTD_ERROR("mark_bad");
	}
	return 0;
}

/*
 * Read input data from (*p_in_buf) (size = (*p_in_len)), adjust both
 * (*p_in_buf) and (*p_in_len) while reading. Write input data to a proper
 * offset within priv->filebuf.
 *
 * On success return 0 and initialize output arguments:
 * - (*p_wbuf) (a pointer within priv->filebuf to write data into flash
 *   starting from).
 * - (*p_to_write) (number of bytes from (*p_wbuf) to write into flash).
 * If necessary append padding (FLASH_EMPTY_BYTE) to the data to be written.
 * (*p_to_write) is guaranteed to be multiple of priv->mtd->min_io_size.
 *
 * On failure return -1. In this case there is nothing to write to flash, all
 * data from (*p_in_buf), has been read and stored in priv->filebuf.
 * Need to wait for next flash_write() call to get more data.
 */
static int read_data(struct flash_priv *priv, const unsigned char **p_in_buf,
		size_t *p_in_len, unsigned char **p_wbuf, int *p_to_write)
{
	int read_available, to_read, write_available, to_write;
	size_t in_len = *p_in_len;

	assert(priv->filebuf_len <= priv->mtd->eb_size);
	assert(priv->writebuf_offset <= priv->filebuf_len);
	assert((priv->writebuf_offset % priv->mtd->min_io_size) == 0);
	assert(priv->imglen >= in_len);

	/* Read as much as possible data from (*p_in_buf) to priv->filebuf: */
	read_available = priv->mtd->eb_size - priv->filebuf_len;
	assert(read_available >= 0);
	to_read = (in_len > read_available) ? read_available : in_len;
	assert(to_read <= read_available);
	assert(to_read <= in_len);
	memcpy(priv->filebuf + priv->filebuf_len, *p_in_buf, to_read);
	*p_in_buf += to_read;
	(*p_in_len) -= to_read;
	priv->filebuf_len += to_read;
	priv->imglen -= to_read;

	if (priv->imglen == 0) {
		/* We've read all image data available.
		 * Add padding to priv->filebuf if necessary: */
		int len, pad_bytes;
		len = ROUND_UP(priv->filebuf_len, priv->mtd->min_io_size);
		assert(len >= priv->filebuf_len);
		assert(len <= priv->mtd->eb_size);
		pad_bytes = len - priv->filebuf_len;
		assert(pad_bytes >= 0);
		erase_buffer(priv->filebuf + priv->filebuf_len, pad_bytes);
		priv->filebuf_len = len;
	}

	write_available = priv->filebuf_len - priv->writebuf_offset;
	assert(write_available >= 0);

	to_write = ROUND_DOWN(write_available, priv->mtd->min_io_size);
	assert(to_write <= write_available);
	assert((to_write % priv->mtd->min_io_size) == 0);

	if (to_write == 0) {
		/* Got not enough data to write. */
		return -1; /* Wait for more data in next flash_write() call. */
	}

	if (priv->is_nand) {
		/* For NAND flash limit amount of data to be written to a
		 * single page. This allows us to skip writing "empty" pages
		 * (filled with FLASH_EMPTY_BYTE).
		 *
		 * Note: for NOR flash typical min_io_size is 1. Writing 1 byte
		 * at time is not practical. */
		to_write = priv->mtd->min_io_size;
	}

	*p_wbuf = priv->filebuf + priv->writebuf_offset;
	*p_to_write = to_write;
	return 0; /* Need to write some data. */
}

/*
 * Check and process current erase block. Return:
 * - 0 if proper erase block has been found.
 * - 1 if current erase block is bad and need to continue the search.
 * - Negative errno code on system error (in this case error reporting is
 *   already done in this function).
 */
static int process_new_erase_block(struct flash_priv *priv)
{
	int ret;

	if (priv->check_bad) {
		int is_bad = mtd_is_bad(priv->mtd, priv->fdout, priv->eb);
		if (is_bad > 0)
			return 1;
		if (is_bad < 0) {
			if (is_not_supported(errno)) {
				/* I don't know whether such cases really
				 * exist.. Let's handle them just in case (as
				 * it is currently implemented in mtd-utils and
				 * in previous version of swupdate). */
				MTD_TRACE_NOT_SUPPORTED("is_bad");
				priv->check_bad = false;
			} else
				return MTD_ERROR("is_bad");
		}
	}

	if (priv->check_locked) {
		int is_locked = mtd_is_locked(priv->mtd, priv->fdout,
		                              priv->eb);
		if (is_locked < 0) {
			if (is_not_supported(errno)) {
				/* Some MTD drivers in linux kernel may not
				 * initialize mtd->_is_locked, hence we expect
				 * these errors in such cases. At the same time
				 * the driver can initialize mtd->_unlock,
				 * hence we shall try to execute mtd_unlock()
				 * in such cases. */
				MTD_TRACE_NOT_SUPPORTED("is_locked");
				priv->check_locked = false;
				priv->do_unlock = true;
			} else
				return MTD_ERROR("is_locked");
		} else
			priv->do_unlock = (bool)is_locked;
	}

	if (priv->do_unlock) {
		ret = mtd_unlock(priv->mtd, priv->fdout, priv->eb);
		if (ret) {
			if (is_not_supported(errno)) {
				/* Some MTD drivers in linux kernel may not
				 * initialize mtd->_unlock, hence we expect
				 * these errors in such cases. */
				MTD_TRACE_NOT_SUPPORTED("unlock");
				priv->check_locked = false;
				priv->do_unlock = false;
			} else
				return MTD_ERROR("unlock");
		}
	}

	/*
	 * NAND flash should always be erased (follow "write once rule").
	 * For other flash types check if the flash is already empty.
	 * In case of NOR flash, it can save a significant amount of time
	 * because erasing a NOR flash is very time expensive.
	 */
	if (!priv->is_nand) {
		ret = mtd_read(priv->mtd, priv->fdout, priv->eb, 0,
		               priv->readout_buf, priv->mtd->eb_size);
		if (ret)
			return MTD_ERROR("read");
		/* Check if already empty: */
		if (buffer_check_pattern(priv->readout_buf, priv->mtd->eb_size,
		                         FLASH_EMPTY_BYTE)) {
			return 0;
		}
	}

	ret = erase_block(priv);
	if (ret) {
		switch (ret) {
		case -EIO:
			ret = mark_bad_block(priv);
			if (ret)
				return ret;
			return 1;
		default:
			return ret;
		}
	}
	return 0;
}

/*
 * Find a non-bad erase block (starting from priv->eb) we can write data into.
 * Unlock the block and erase it if necessary.
 * As a result update (if necessary) priv->eb to point to a block we can write
 * data into.
 */
static int prepare_new_erase_block(struct flash_priv *priv)
{
	for (; priv->eb < priv->eb_end; priv->eb++) {
		int ret = process_new_erase_block(priv);
		if (ret < 0)
			return ret;
		else if (ret == 0)
			return 0;
	}
	return too_many_bad_blocks(priv->mtdnum);
}

/*
 * A callback to be passed to copyimage().
 * Write as much input data to flash as possible at this time.
 * Skip existing and detect new bad blocks (mark them as bad) while writing.
 * Store leftover data (if any) in private context (priv). The leftover data
 * will be written into flash during next flash_write() executions.
 * During last flash_write() execution all image data should be written to
 * flash (the data should be padded with FLASH_EMPTY_BYTE if necessary).
 */
static int flash_write(void *out, const void *buf, size_t len)
{
	int ret;
	struct flash_priv *priv = (struct flash_priv *)out;
	const unsigned char **pbuf = (const unsigned char**)&buf;

	while ((len > 0) || (priv->writebuf_offset < priv->filebuf_len)) {
		unsigned char *wbuf;
		int to_write;

		assert(priv->eb <= priv->eb_end);
		if (priv->eb == priv->eb_end)
			return too_many_bad_blocks(priv->mtdnum);

		ret = read_data(priv, pbuf, &len, &wbuf, &to_write);
		if (ret < 0) {
			/* Wait for more data to be written in next
			 * flash_write() call. */
			break;
		}
		/* We've got some data to be written to flash. */

		if (priv->writebuf_offset == 0) {
			/* Start of a new erase block. */
			ret = prepare_new_erase_block(priv);
			if (ret)
				return ret;
		}
		/* Now priv->eb points to a valid erased block we can write
		 * data into. */

		ret = buffer_check_pattern(wbuf, to_write, FLASH_EMPTY_BYTE);
		if (ret) {
			ret = 0; /* There is no need to write "empty" bytes. */
		} else {
			/* Write data to flash: */
			ret = mtd_write(priv->libmtd, priv->mtd, priv->fdout,
			                priv->eb, priv->writebuf_offset, wbuf,
			                to_write, NULL, 0, MTD_OPS_PLACE_OOB);
		}
		if (ret) {
			if (errno != EIO)
				return MTD_ERROR("write");

			ret = erase_block(priv);
			if (ret) {
				if (ret != -EIO)
					return ret;
			}

			ret = mark_bad_block(priv);
			if (ret)
				return ret;
			/* Rewind to erase block start. */
			priv->writebuf_offset = 0;
			priv->eb++;
			continue;
		}

		priv->writebuf_offset += to_write;
		assert(priv->writebuf_offset <= priv->filebuf_len);
		assert(priv->filebuf_len <= priv->mtd->eb_size);
		if (priv->writebuf_offset == priv->mtd->eb_size) {
			priv->eb++;
			priv->writebuf_offset = 0;
			priv->filebuf_len = 0;
		}
	}

	return 0;
}

static int flash_write_image(int mtdnum, struct img_type *img)
{
	struct flash_priv priv;
	char mtd_device[LINESIZE];
	int ret;
	struct flash_description *flash = get_flash_info();
	priv.mtd = &flash->mtd_info[mtdnum].mtd;
	assert((priv.mtd->eb_size % priv.mtd->min_io_size) == 0);

	if (!mtd_dev_present(flash->libmtd, mtdnum)) {
		ERROR("MTD %d does not exist", mtdnum);
		return -ENODEV;
	}

	if (img->seek & (priv.mtd->eb_size - 1)) {
		ERROR("The start address is not erase-block-aligned!\n"
		      "The erase block of this flash is 0x%x.\n",
		      priv.mtd->eb_size);
		return -EINVAL;
	}

	priv.imglen = get_output_size(img, true);
	if (priv.imglen < 0) {
		WARN("Failed to determine output size, getting MTD size.");
		priv.imglen = get_mtd_size(mtdnum);
		if (priv.imglen < 0) {
			ERROR("Could not get MTD %d device size", mtdnum);
			return -ENODEV;
		}
	}

	if (!priv.imglen)
		return 0;

	if (priv.imglen > priv.mtd->size - img->seek) {
		ERROR("Image %s does not fit into mtd%d", img->fname, mtdnum);
		return -ENOSPC;
	}

	snprintf(mtd_device, sizeof(mtd_device), "/dev/mtd%d", mtdnum);
	priv.fdout = open(mtd_device, O_RDWR);
	if (priv.fdout < 0) {
		ret = errno;
		ERROR( "%s: %s: %s", __func__, mtd_device, STRERROR(ret));
		return -ret;
	}

	priv.mtdnum = mtdnum;
	priv.filebuf_len = 0;
	priv.writebuf_offset = 0;
	priv.eb = (int)(img->seek / priv.mtd->eb_size);
	priv.eb_end = (int)(priv.mtd->size / priv.mtd->eb_size);
	priv.libmtd = flash->libmtd;
	priv.is_nand = isNand(flash, mtdnum);
	priv.check_bad = true;
	priv.check_locked = true;

	ret = priv.mtd->eb_size;
	if (!priv.is_nand)
		ret += priv.mtd->eb_size;
	priv.filebuf = malloc(ret);
	if (!priv.filebuf) {
		ERROR("Failed to allocate %d bytes of memory", ret);
		ret = -ENOMEM;
		goto end;
	}
	if (!priv.is_nand)
		priv.readout_buf = priv.filebuf + priv.mtd->eb_size;

	ret = copyimage(&priv, img, flash_write);
	free(priv.filebuf);

end:
	if (close(priv.fdout)) {
		if (!ret)
			ret = -errno;
		ERROR("close() failed: %s", STRERROR(errno));
	}

	/* tell 'nbytes == 0' (EOF) from 'nbytes < 0' (read error) */
	if (ret < 0) {
		ERROR("Failure installing into: %s", img->device);
		return ret;
	}
	return 0;
}

static int install_flash_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	int ret, mtdnum;

	if (strlen(img->mtdname))
		mtdnum = get_mtd_from_name(img->mtdname);
	else
		mtdnum = get_mtd_from_device(img->device);
	if (mtdnum < 0) {
		ERROR("Wrong MTD device in description: %s",
			strlen(img->mtdname) ? img->mtdname : img->device);
		return -EINVAL;
	}

	TRACE("Copying %s into /dev/mtd%d", img->fname, mtdnum);
	ret = flash_write_image(mtdnum, img);
	if (ret) {
		ERROR("I cannot copy %s into %s partition",
			img->fname,
			img->device);
		return ret;
	}

	return 0;
}

__attribute__((constructor))
void flash_handler(void)
{
	register_handler("flash", install_flash_image,
				IMAGE_HANDLER | FILE_HANDLER, NULL);
}
