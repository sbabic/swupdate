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


#ifndef _FLASH_PART_H
#define _FLASH_PART_H

#include <stdint.h>
#include <mtd/libmtd.h>
#include <mtd/libubi.h>
#include "bsdqueue.h"

#define DEFAULT_CTRL_DEV "/dev/ubi_ctrl"
#define UBI_DATA_VOLNAME	"data"
#define UBI_DATACPY_VOLNAME	"datacpy"
#define MAX_MTD_DEVICES		10
#define MTD_FS_DEVICE		7

struct ubi_part {
	struct ubi_vol_info vol_info;
	LIST_ENTRY(ubi_part) next;
};

LIST_HEAD(ubilist, ubi_part);

struct mtd_ubi_info {
	struct ubi_dev_info dev_info;
	struct ubilist ubi_partitions;
	struct ubi_attach_request req;
	struct mtd_dev_info mtd;
	int skipubi;
	int scanned;
};

struct flash_description {
	libubi_t libubi;
	libmtd_t libmtd;
	struct ubi_info ubi_info;
	struct mtd_info mtd;
	struct mtd_ubi_info *mtd_info;
};

void ubi_mount(struct ubi_vol_info *vol, const char *mntpoint);
void ubi_umount(const char *mntpoint);

void mtd_init(void);
void mtd_set_ubiblacklist(char *mtdlist);
void ubi_init(void);
int scan_mtd_devices (void);
void mtd_cleanup (void);
int get_mtd_from_device(char *s);

struct flash_description *get_flash_info(void);
#define isNand(flash, index) \
	(flash->mtd_info[index].mtd.type == MTD_NANDFLASH || \
	 flash->mtd_info[index].mtd.type == MTD_MLCNANDFLASH)

#endif
