/*
 * (C) Copyright 2014
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */


#ifndef _FLASH_PART_H
#define _FLASH_PART_H

#include <stdint.h>
#include <libmtd.h>
#include <libubi.h>
#include "bsdqueue.h"

#define DEFAULT_CTRL_DEV "/dev/ubi_ctrl"

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
	int skipubi;	/* set if no UBI scan must run */
	int has_ubi;	/* set if MTD must always have UBI */
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
int get_mtd_from_name(const char *s);
int flash_erase(int mtdnum);

struct flash_description *get_flash_info(void);
#define isNand(flash, index) \
	(flash->mtd_info[index].mtd.type == MTD_NANDFLASH || \
	 flash->mtd_info[index].mtd.type == MTD_MLCNANDFLASH)

#endif
