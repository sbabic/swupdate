/*
 * (C) Copyright 2012
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
 * Foundation, Inc. 
 */

#ifndef _UBI_PART_H
#define _UBI_PART_H

#include <sys/queue.h>
#include <mtd/libubi.h>
#include <mtd/libmtd.h>
#include "mtd_config.h"

#define DEFAULT_CTRL_DEV "/dev/ubi_ctrl"
#define UBI_DATA_VOLNAME	"data"
#define UBI_DATACPY_VOLNAME	"datacpy"

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
	int scanned;
};

struct flash_description {
	libubi_t libubi;
	libmtd_t libmtd;
	struct ubi_info ubi_info;
	struct mtd_info mtd;
	struct mtd_ubi_info mtd_info[MAX_MTD_DEVICES];
};

#if defined(CONFIG_UBIVOL)
void scan_ubi_partitions(int mtd);
void ubi_init(void);
void ubi_mount(struct ubi_vol_info *vol, const char *mntpoint);
void ubi_umount(const char *mntpoint);
int scan_mtd_devices (void);
void mtd_cleanup (void);
void mtd_init(void);
struct flash_description *get_flash_info(void);
struct ubi_part *search_volume(const char *str, struct ubilist *list);
#endif

extern struct flash_description *get_flash_info(void);

#endif
