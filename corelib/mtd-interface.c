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
#include "ubi_partition.h"
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
		if (!flash->mtd_info[i].skipubi)
			scan_ubi_partitions(i);
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
