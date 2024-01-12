/*
 * (C) Copyright 2017
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
#include <linux/version.h>
#include <sys/ioctl.h>

#include <mtd/mtd-user.h>
#include "swupdate_image.h"
#include "handler.h"
#include "util.h"
#include "flash.h"
#include "progress.h"

#define PROCMTD	"/proc/mtd"
#define LINESIZE	80

#define EVEN_WHOLE  0xff
#define EVEN_HALF   0x0f
#define ODD_HALF    0xf0
#define EVEN_FOURTH 0x33
#define ODD_FOURTH  0xcc
#define EVEN_EIGHTH 0x55
#define ODD_EIGHTH  0xaa
#define ODD_WHOLE   0x00

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,1,0)
#define MTD_FILE_MODE_RAW MTD_MODE_RAW
#endif

#define _L1(n)  (((n) < 2)     ?      0 :  1)
#define _L2(n)  (((n) < 1<<2)  ? _L1(n) :  2 + _L1((n)>>2))
#define _L4(n)  (((n) < 1<<4)  ? _L2(n) :  4 + _L2((n)>>4))
#define _L8(n)  (((n) < 1<<8)  ? _L4(n) :  8 + _L4((n)>>8))
#define LOG2(n) (((n) < 1<<16) ? _L8(n) : 16 + _L8((n)>>16))

void flash_1bit_hamming_handler(void);

static unsigned char calc_bitwise_parity(unsigned char val, unsigned char mask)
{
	unsigned char result = 0, byte_mask;
	int i;

	byte_mask = mask;

	for (i = 0; i < 8; i++) {
		if ((byte_mask & 0x1) != 0)
			result ^= (val & 1);
		byte_mask >>= 1;
		val >>= 1;
	}
	return result & 0x1;
}

static unsigned char calc_row_parity_bits(unsigned char byte_parities[],
					  int even, int chunk_size,
					  int sector_size)
{
	unsigned char result = 0;
	int i, j;

	for (i = (even ? 0 : chunk_size);
	     i < sector_size;
	     i += (2 * chunk_size)) {
		for (j = 0; j < chunk_size; j++)
			result ^= byte_parities[i + j];
	}
	return result & 0x1;
}

/*
 * Based on Texas Instrument's C# GenECC application
 * (sourceforge.net/projects/dvflashutils)
 */
static unsigned int nand_calculate_ecc(unsigned char *buf, unsigned int sector_size)
{
	unsigned short odd_result = 0, even_result = 0;
	unsigned char bit_parities = 0;
	int i;
	unsigned char val;
	unsigned char *byte_parities = malloc(sector_size);

	if (!byte_parities)
		return -ENOMEM;

	for (i = 0; i < sector_size; i++)
		bit_parities ^= buf[i];

	even_result |= ((calc_bitwise_parity(bit_parities, EVEN_HALF) << 2) |
			(calc_bitwise_parity(bit_parities, EVEN_FOURTH) << 1) |
			(calc_bitwise_parity(bit_parities, EVEN_EIGHTH) << 0));

	odd_result |= ((calc_bitwise_parity(bit_parities, ODD_HALF) << 2) |
			(calc_bitwise_parity(bit_parities, ODD_FOURTH) << 1) |
			(calc_bitwise_parity(bit_parities, ODD_EIGHTH) << 0));

	for (i = 0; i < sector_size; i++)
		byte_parities[i] = calc_bitwise_parity(buf[i], EVEN_WHOLE);

	for (i = 0; i < LOG2(sector_size); i++) {
		val = 0;
		val = calc_row_parity_bits(byte_parities, 1, 1 << i, sector_size);
		even_result |= (val << (3 + i));

		val = calc_row_parity_bits(byte_parities, 0, 1 << i, sector_size);
		odd_result |= (val << (3 + i));
	}

	free(byte_parities);

	return (odd_result << 16) | even_result;
}

static int write_ecc(int ofd, unsigned char *ecc, int start)
{
	struct mtd_oob_buf oob;
	unsigned char oobbuf[64];
	int i;

	memset(oobbuf, 0xff, sizeof(oobbuf));

	for (i = 0; i < 12; i++)
		oobbuf[i + 2] = ecc[i];

	oob.start = start;
	oob.ptr = oobbuf;
	oob.length = 64;

	return ioctl(ofd, MEMWRITEOOB, &oob) != 0;
}

static void ecc_sector(unsigned char *sector, unsigned char *code,
			unsigned int sector_size)
{
	unsigned char *p;
	int ecc = 0;

	ecc = nand_calculate_ecc(sector, sector_size);

	p = (unsigned char *) &ecc;

	code[0] = p[0];
	code[1] = p[2];
	code[2] = p[1] | (p[3] << 4);
}

static int flash_write_nand_hamming1(int mtdnum, struct img_type *img)
{
	struct flash_description *flash = get_flash_info();
	struct mtd_dev_info *mtd = &flash->mtd_info[mtdnum].mtd;
	int fd = img->fdin;
	int ofd;
	unsigned char *page;
	unsigned char code[3];
	unsigned char ecc[12];
	int cnt;
	int i, j;
	int len;
	long long imglen = 0;
	int page_idx = 0;
	int ret = EXIT_FAILURE;
	char mtd_device[LINESIZE];
	bool rawNand = isNand(flash, mtdnum);

	/*
	 * if nothing to do, returns without errors
	 */
	if (!img->size)
		return 0;

	snprintf(mtd_device, sizeof(mtd_device), "/dev/mtd%d", mtdnum);

	/*
	 * Get page size
	 */
	len = mtd->min_io_size;
	if (!rawNand)
		len *= 2;

	imglen = img->size;

	page = (unsigned char *) malloc(len);
	if (page == NULL) {
		ERROR("Error opening input file");
		goto out;
	}

	ofd = open(mtd_device, O_CREAT | O_RDWR, S_IRWXU | S_IRWXG);
	if (ofd < 0) {
		ERROR("Error opening output file");
		goto out_input;
	}

	if (rawNand)
		/* The device has to be accessed in RAW mode to fill oob area */
		if (ioctl(ofd, MTDFILEMODE, (void *) MTD_FILE_MODE_RAW)) {
			ERROR("RAW mode access");
			goto out_input;
		}

	while (imglen > 0) {
		cnt = read(fd, page, min(mtd->min_io_size, imglen));
		if (cnt < 0)
			break;

		/* Writes has to be page aligned */
		if (cnt < mtd->min_io_size)
			memset(page + cnt, 0xff, mtd->min_io_size - cnt);

		if (rawNand)
			for (i = 0; i < mtd->min_io_size / mtd->subpage_size; i++) {
				/* Obtain ECC code for sector */
				ecc_sector(page + i * mtd->subpage_size, code, mtd->subpage_size);
				for (j = 0; j < 3; j++)
					ecc[i * 3 + j] = code[j];
			}
		else
			/* The OneNAND has a 2-plane memory but the ROM boot
			 * can only access one of them, so we have to double
			 * copy each 2K page. */
			memcpy(page + mtd->min_io_size, page, mtd->min_io_size);

		if (write(ofd, page, len) != len) {
			perror("Error writing to output file");
			goto out_output;
		}

		if (rawNand)
			if (write_ecc(ofd, ecc, page_idx * mtd->min_io_size)) {
				perror("Error writing ECC in OOB area");
				goto out_output;
			}
		page_idx++;

		imglen -= cnt;

		/*
		 * this handler does not use copyfile()
		 * and must update itself the progress bar
		 */
		swupdate_progress_update((img->size - imglen) * 100 / img->size);
	}

	if (cnt < 0) {
		ERROR("File I/O error on input file");
		goto out_output;
	}

	TRACE("Successfully written %s to mtd %d", img->fname, mtdnum);
	ret = EXIT_SUCCESS;

out_output:
	close(ofd);
out_input:
	free(page);
out:
	return ret;
}

static int install_flash_hamming_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	int mtdnum;

	if (strlen(img->mtdname))
		mtdnum = get_mtd_from_name(img->mtdname);
	else
		mtdnum = get_mtd_from_device(img->device);
	if (mtdnum < 0) {
		ERROR("Wrong MTD device in description: %s",
			strlen(img->mtdname) ? img->mtdname : img->device);
		return -1;
	}

	if(flash_erase(mtdnum)) {
		ERROR("I cannot erasing %s",
			img->device);
		return -1;
	}
	TRACE("Copying %s into /dev/mtd%d", img->fname, mtdnum);
	if (flash_write_nand_hamming1(mtdnum, img)) {
		ERROR("I cannot copy %s into %s partition",
			img->fname,
			img->device);
		return -1;
	}

	return 0;
}

__attribute__((constructor))
void flash_1bit_hamming_handler(void)
{
	register_handler("flash-hamming1", install_flash_hamming_image,
				IMAGE_HANDLER | FILE_HANDLER,  (void *)1);
}
