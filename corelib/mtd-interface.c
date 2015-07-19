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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mtd/mtd-user.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <unistd.h>
#include <errno.h>
#include "bsdqueue.h"
#include "util.h"
#include "flash.h"

static char mtd_ubi_blacklist[100] = { 0 };

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

void mtd_set_ubiblacklist(char *mtdlist)
{
	strncpy(mtd_ubi_blacklist, mtdlist, sizeof(mtd_ubi_blacklist));
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

	/* Blacklist passed on the command line has priority */
	if (strlen(mtd_ubi_blacklist))
		strncpy(blacklist, mtd_ubi_blacklist, sizeof(blacklist));

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

	/* Allocate memory to store MTD infos */
	flash->mtd_info = (struct mtd_ubi_info *)calloc(
				mtd_info->highest_mtd_num + 1,
				sizeof(struct mtd_ubi_info));
	if (!flash->mtd_info) {
		ERROR("No enough memory for MTD structures");
		return -ENOMEM;
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
		if (flash->libubi && !flash->mtd_info[i].skipubi)
			scan_ubi_partitions(i);
#endif
	}

	return mtd_info->mtd_dev_cnt;
}

void scan_ubi_partitions(int mtd)
{
	struct flash_description *nand = get_flash_info();
	int err;
	libubi_t libubi = nand->libubi;
	struct ubi_part *ubi_part;
	struct mtd_ubi_info *mtd_info;
	int i;

	if (mtd < 0 || mtd > MAX_MTD_DEVICES)
		ERROR("wrong MTD device /dev/mtd%d", mtd);

	mtd_info = &nand->mtd_info[mtd];
	LIST_INIT(&mtd_info->ubi_partitions);

	/*
	 * The program is called directly after a boot,
	 * and a detach is not required. However,
	 * detaching at the beginning allows consecutive
	 * start of the program itself
	 */
	ubi_detach_mtd(libubi, DEFAULT_CTRL_DEV, mtd);

	mtd_info->req.dev_num = UBI_DEV_NUM_AUTO;
	mtd_info->req.mtd_num = mtd;
#if defined(CONFIG_UBIVIDOFFSET)
	mtd_info->req.vid_hdr_offset = CONFIG_UBIVIDOFFSET;
#else
	mtd_info->req.vid_hdr_offset = 0;
#endif
	mtd_info->req.mtd_dev_node = NULL;

	err = ubi_attach(libubi, DEFAULT_CTRL_DEV, &mtd_info->req);
	if (err) {
		TRACE("cannot attach mtd%d - maybe not a NAND or raw device", mtd);
		return;
	}

	err = ubi_get_dev_info1(libubi, mtd_info->req.dev_num, &mtd_info->dev_info);
	if (err) {
		TRACE("cannot get information about UBI device %d", mtd_info->req.dev_num);
		return;
	}

	for (i = mtd_info->dev_info.lowest_vol_id;
	     i <= mtd_info->dev_info.highest_vol_id; i++) {
		ubi_part = (struct ubi_part *)calloc(1, sizeof(struct ubi_part));
		if (!ubi_part)
			ERROR("No memory: malloc failed\n");

		err = ubi_get_vol_info1(libubi, mtd_info->dev_info.dev_num, i, &ubi_part->vol_info);
		if (err == -1) {
			if (errno == ENOENT)
				continue;

			TRACE("libubi failed to probe volume %d on ubi%d",
					  i, mtd_info->dev_info.dev_num);
			return;
		}

		LIST_INSERT_HEAD(&mtd_info->ubi_partitions, ubi_part, next);
		TRACE("mtd%d:\tVolume found : \t%s",
			mtd,
			ubi_part->vol_info.name);
	}

	mtd_info->scanned = 1;
}

void ubi_mount(struct ubi_vol_info *vol, const char *mntpoint)
{
	int ret;
	char node[64];

	snprintf(node, sizeof(node), "/dev/ubi%d_%d",
		vol->dev_num,
		vol->vol_id);

	ret = mount(node, mntpoint,
                 "ubifs", 0, NULL);

	if (ret)
		ERROR("UBIFS cannot be mounted : device %s volume %s on %s : %s",
			node, vol->name, mntpoint, strerror(errno));
}

void ubi_umount(const char *mntpoint)
{
	umount(mntpoint);
}

void mtd_cleanup (void)
{
	int i;
	struct ubilist *list;
	struct ubi_part *vol;
	struct flash_description *flash = get_flash_info();

	if (flash->mtd_info) {
		for (i = flash->mtd.lowest_mtd_num; i <= flash->mtd.highest_mtd_num; i++) {
			list = &flash->mtd_info[i].ubi_partitions;
			LIST_FOREACH(vol, list, next) {
				LIST_REMOVE(vol, next);
				free(vol);
			}
		}
		free(flash->mtd_info);
		flash->mtd_info = NULL;
	}

	/* Do not clear libraries handles */
	memset(&flash->ubi_info, 0, sizeof(struct ubi_info));
	memset(&flash->mtd, 0, sizeof(struct mtd_info));
}
