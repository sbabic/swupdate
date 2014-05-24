/*
 * (C) Copyright 2014
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mtd/mtd-user.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <unistd.h>
#include <errno.h>
#include "util.h"
#include "flash.h"

void mtd_init(void)
{
	struct flash_description *flash = get_flash_info();
	flash->libmtd = libmtd_open();
	if (flash->libmtd == NULL) {
		if (errno == 0)
			ERROR("MTD is not present in the system");
		ERROR("cannot open libmtd");
	}
}

int get_mtd_from_device(char *s) {
	int ret;
	int mtdnum;

	if (!s)
		return -1;

	ret = sscanf(s, "mtd%d", &mtdnum);
	if (ret <= 0)
		ret = sscanf(s, "/dev/mtd%d", &mtdnum);

	if (ret <= 0)
		return -1;

	return mtdnum;
}

void ubi_init(void)
{
	struct flash_description *nand = get_flash_info();
	int err;
	libubi_t libubi;

	libubi = libubi_open();
	if (!libubi) {
		if (errno == 0)
			ERROR("UBI is not present in the system");
		ERROR("cannot open libubi");
		return;
	}

	nand->libubi = libubi;

	err = ubi_get_info(libubi, &nand->ubi_info);
	if (err) {
		ERROR("cannot get UBI information");
		return;
	}

	if (nand->ubi_info.ctrl_major == -1) {
		ERROR("MTD attach/detach feature is not supported by your kernel");
	}
}

static void ubi_insert_blacklist(int index, struct flash_description *flash)
{
	struct mtd_info *mtd = &flash->mtd;

	if (index >= mtd->lowest_mtd_num && index <= mtd->highest_mtd_num) {
		flash->mtd_info[index].skipubi = 1;
	}
}

int scan_mtd_devices (void)
{
	int err;
	struct flash_description *flash = get_flash_info();
	struct mtd_info *mtd_info = &flash->mtd;
	libmtd_t libmtd = flash->libmtd;
	char blacklist[100] = { 0 };
	char *token;
	char *saveptr;
	int i, index;

#if defined(CONFIG_UBIBLACKLIST)
	strncpy(blacklist, CONFIG_UBIBLACKLIST, sizeof(blacklist));
#endif

	if (!libmtd) {
		ERROR("MTD is not present on the target");
		return -1;
	}
	err = mtd_get_info(libmtd, mtd_info);
	if (err) {
		if (errno == ENODEV)
			ERROR("MTD is not present on the board");
		return 0;
	}
	token = strtok_r(blacklist, " ", &saveptr);
	if (token) {
		errno = 0;
		index = strtoul(token, NULL, 10);
		if (errno == 0) {
			ubi_insert_blacklist(index, flash);

			while ((token = strtok_r(NULL, " ", &saveptr))) {
				errno = 0;
				index = strtoul(token, NULL, 10);
				if (errno != 0)
					break;
				ubi_insert_blacklist(index, flash);
			}
		}
	}

	for (i = mtd_info->lowest_mtd_num;
	     i <= mtd_info->highest_mtd_num; i++) {
		if (!mtd_dev_present(libmtd, i))
			continue;
		err = mtd_get_dev_info1(libmtd, i, &flash->mtd_info[i].mtd);
		if (err) {
			TRACE("No information from MTD%d", i);
			continue;
		}
#if defined(CONFIG_UBIVOL)
		if (!flash->mtd_info[i].skipubi)
			scan_ubi_partitions(i);
#endif
	}

	return mtd_info->mtd_dev_cnt;
}

void mtd_cleanup (void)
{
	int i;
	struct ubilist *list;
	struct ubi_part *vol;
	struct flash_description *flash = get_flash_info();

	for (i = flash->mtd.lowest_mtd_num; i <= flash->mtd.highest_mtd_num; i++) {
		list = &flash->mtd_info[i].ubi_partitions;
		LIST_FOREACH(vol, list, next) {
			LIST_REMOVE(vol, next);
			free(vol);
		}
	}

	/* Do not clear libraries handles */
	memset(&flash->ubi_info, 0, sizeof(struct ubi_info));
	memset(&flash->mtd, 0, sizeof(struct mtd_info));
	memset(&flash->mtd_info, 0, sizeof(flash->mtd_info));
}
