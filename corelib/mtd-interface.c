/*
 * (C) Copyright 2014
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <string.h>
#include <mtd/mtd-user.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include "bsdqueue.h"
#include "util.h"
#include "flash.h"

static char mtd_ubi_blacklist[100] = { 0 };

/*
 * Note: the functions here are derived directly
 * with minor changes from mtd-utils.
 */
#define EMPTY_BYTE	0xFF

int flash_erase(int mtdnum)
{
	int fd;
	char mtd_device[80];
	struct mtd_dev_info *mtd;
	int noskipbad = 0;
	int ret = 0;
	unsigned int eb, eb_start, eb_cnt, i;
	uint8_t *buf;
	struct flash_description *flash = get_flash_info();

	if  (!mtd_dev_present(flash->libmtd, mtdnum)) {
			ERROR("MTD %d does not exist", mtdnum);
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
	buf = (uint8_t *)malloc(mtd->eb_size);
	if (!buf) {
		ERROR("No memory for temporary buffer of %d bytes",
			mtd->eb_size);
		close(fd);
		return -ENOMEM;
	}

	eb_start = 0;
	eb_cnt = (mtd->size / mtd->eb_size) - eb_start;
	for (eb = 0; eb < eb_start + eb_cnt; eb++) {

		/* Always skip bad sectors */
		if (!noskipbad) {
			int isbad = mtd_is_bad(mtd, fd, eb);
			if (isbad > 0) {
				continue;
			} else if (isbad < 0) {
				if (errno == EOPNOTSUPP) {
					noskipbad = 1;
				} else {
					ERROR("%s: MTD get bad block failed", mtd_device);
					ret  = -EFAULT;
					goto erase_out;
				}
			}
		}

		/* Unlock memory if required */
		if (mtd_is_locked(mtd, fd, eb)) {
			if (mtd_unlock(mtd, fd, eb) != 0) {
				if (errno != EOPNOTSUPP) {
					TRACE("%s: MTD unlock failure", mtd_device);
					continue;
				}
			}
		}

		/*
		 * In case of NOR flash, check if the flash
		 * is already empty. This can save
		 * an amount of time because erasing
		 * a NOR flash is very time expensive.
		 * NAND flash is always erased.
		 */
		if (!isNand(flash, mtdnum)) {
			if (mtd_read(mtd, fd, eb, 0, buf, mtd->eb_size) != 0) {
				ERROR("%s: MTD Read failure", mtd_device);
				ret  = -EIO;
				goto erase_out;
			}

			/* check if already empty */
			for (i = 0; i < mtd->eb_size; i++) {
				if (buf[i] != EMPTY_BYTE)
					break;
			}

			/* skip erase if empty */
			if (i == mtd->eb_size)
				continue;

		}

		/* The sector contains data and it must be erased */
		if (mtd_erase(flash->libmtd, mtd, fd, eb) != 0) {
			ERROR("%s: MTD Erase failure", mtd_device);
			ret  = -EIO;
			goto erase_out;
		}
	}

erase_out:
	free(buf);

	close(fd);

	return ret;
}


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
	char *real_s;

	if (!s)
		return -1;

	real_s = realpath(s, NULL);
	if (real_s == NULL) {
		char tmp_s[PATH_MAX] = {0};

		if (! strncmp(s, "/dev/", 5))
			return -1;

		snprintf(tmp_s, sizeof(tmp_s), "/dev/%s", s);
		real_s = realpath(tmp_s, NULL);
		if (real_s == NULL)
			return -1;
	}

	TRACE("mtd name [%s] resolved to [%s]", s, real_s);
	ret = sscanf(real_s, "mtd%d", &mtdnum);
	if (ret <= 0)
		ret = sscanf(real_s, "/dev/mtd%d", &mtdnum);

	free (real_s);

	if (ret <= 0)
		return -1;

	return mtdnum;
}

int get_mtd_from_name(const char *s)
{
	struct flash_description *flash = get_flash_info();
	struct mtd_dev_info *info;
	int i;

	for (i = flash->mtd.lowest_mtd_num;
	     i <= flash->mtd.highest_mtd_num; i++) {
		info = &flash->mtd_info[i].mtd;
		if (!strcmp(info->name, s))
			return i;
	}

	return -1;
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

static void ubi_insert_list(int index, struct flash_description *flash, bool black)
{
	struct mtd_info *mtd = &flash->mtd;

	if (index >= mtd->lowest_mtd_num && index <= mtd->highest_mtd_num) {
		if (black) {
			flash->mtd_info[index].skipubi = 1;
			flash->mtd_info[index].has_ubi = 0;
		} else {
			flash->mtd_info[index].skipubi = 0;
			flash->mtd_info[index].has_ubi = 1;
		}
	}
}

#if defined(CONFIG_UBIVOL)
static void scan_ubi_volumes(struct mtd_ubi_info *info)
{
	struct flash_description *flash = get_flash_info();
	libubi_t libubi = flash->libubi;
	struct ubi_part *ubi_part;
	int i, err;

	for (i = info->dev_info.lowest_vol_id;
	     i <= info->dev_info.highest_vol_id; i++) {
		ubi_part = (struct ubi_part *)calloc(1, sizeof(struct ubi_part));
		if (!ubi_part) {
			ERROR("No memory: malloc failed");
			return;
		}

		err = ubi_get_vol_info1(libubi, info->dev_info.dev_num,
					i, &ubi_part->vol_info);
		if (err == -1) {
			if (errno == ENOENT || errno == ENODEV)
				continue;

			ERROR("libubi failed to probe volume %d on ubi%d",
					  i, info->dev_info.dev_num);
			return;
		}

		LIST_INSERT_HEAD(&info->ubi_partitions, ubi_part, next);
		TRACE("mtd%d:\tVolume found : \t%s",
			info->dev_info.mtd_num,
			ubi_part->vol_info.name);
	}

	info->scanned = 1;
}

static void scan_for_ubi_devices(void)
{
	struct flash_description *flash = get_flash_info();
	libubi_t libubi = flash->libubi;
	struct ubi_info ubi_info;
	struct ubi_dev_info dev_info;
	struct mtd_ubi_info *mtd_info;
	int err, i, mtd;

	if (!libubi)
		return;
	/*
	 * if not yet an attached device, return and try later
	 * to attach them
	 */
	err = ubi_get_info(libubi, &ubi_info);
	if (err)
		return;

	for (i = ubi_info.lowest_dev_num;
	     i <= ubi_info.highest_dev_num; i++) {
		err = ubi_get_dev_info1(libubi, i, &dev_info);
		if (err == -1) {
			continue;
		}
		mtd = dev_info.mtd_num;
		mtd_info = &flash->mtd_info[mtd];
		if (mtd < 0 || flash->mtd_info[mtd].skipubi)
			continue;
		memcpy(&mtd_info->dev_info, &dev_info, sizeof(struct ubi_dev_info));

		scan_ubi_volumes(mtd_info);
	}
}

#if defined(CONFIG_UBIATTACH)
static void scan_ubi_partitions(int mtd)
{
	struct flash_description *flash = get_flash_info();
	libubi_t libubi = flash->libubi;
	int err, tryattach = 0;
	struct mtd_ubi_info *mtd_info;

	if (mtd < 0) {
		ERROR("wrong MTD device /dev/mtd%d", mtd);
		return;
	}

	mtd_info = &flash->mtd_info[mtd];

	/*
	 * The program is called directly after a boot,
	 * and a detach is not required. However,
	 * detaching at the beginning allows consecutive
	 * start of the program itself
	 */
	mtd_info->req.dev_num = UBI_DEV_NUM_AUTO;
	mtd_info->req.mtd_num = mtd;
#if defined(CONFIG_UBIVIDOFFSET)
	mtd_info->req.vid_hdr_offset = CONFIG_UBIVIDOFFSET;
#else
	mtd_info->req.vid_hdr_offset = 0;
#endif
	mtd_info->req.mtd_dev_node = NULL;

	/*
	 * Check if the MTD was alrady attached
	 * and tries to get information, if not found
	 * try to attach.
	 */
	do {
		err = ubi_attach(libubi, DEFAULT_CTRL_DEV, &mtd_info->req);
		if (err) {
			if (mtd_info->has_ubi && !tryattach) {
				TRACE("cannot attach mtd%d ..try erasing", mtd);
				if (flash_erase(mtd)) {
					ERROR("mtd%d cannot be erased", mtd);
					return;
				}
			} else {
				ERROR("cannot attach mtd%d - maybe not a NAND or raw device", mtd);
				return;
			}
			tryattach++;
		}
	} while (err != 0 && tryattach < 2);

	err = ubi_get_dev_info1(libubi, mtd_info->req.dev_num, &mtd_info->dev_info);
	if (err) {
		ERROR("cannot get information about UBI device %d", mtd_info->req.dev_num);
		return;
	}

	scan_ubi_volumes(mtd_info);
}
#endif
#endif

int scan_mtd_devices (void)
{
	int err;
	struct flash_description *flash = get_flash_info();
	struct mtd_info *mtd_info = &flash->mtd;
	struct mtd_ubi_info *mtd_ubi_info;
	libmtd_t libmtd = flash->libmtd;
	char list[100];
	char *token;
	char *saveptr;
	int i, index;
	bool black;

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

	for (i = 0; i < 2; i++) {
		memset(list, 0, sizeof(list));
		switch (i) {
		case 0:
			black = true;
#if defined(CONFIG_UBIBLACKLIST)
			strncpy(list, CONFIG_UBIBLACKLIST,
				sizeof(list));
#endif
			/* Blacklist passed on the command line has priority */
			if (strlen(mtd_ubi_blacklist))
				strncpy(list, mtd_ubi_blacklist, sizeof(list));
			break;
		case 1:
			black = false;
#if defined(CONFIG_UBIWHITELIST)
			strncpy(list, CONFIG_UBIWHITELIST,
				sizeof(list));
#endif
			break;
		}

		token = strtok_r(list, " ", &saveptr);
		if (token) {
			errno = 0;
			index = strtoul(token, NULL, 10);
			if (errno == 0) {
				ubi_insert_list(index, flash, black);

				while ((token = strtok_r(NULL, " ", &saveptr))) {
					errno = 0;
					index = strtoul(token, NULL, 10);
					if (errno != 0)
						break;
					ubi_insert_list(index, flash, black);
				}
			}
		}
	}

	for (i = mtd_info->lowest_mtd_num;
	     i <= mtd_info->highest_mtd_num; i++) {
		/* initialize data */
		mtd_ubi_info = &flash->mtd_info[i];
		LIST_INIT(&mtd_ubi_info->ubi_partitions);
		if (!mtd_dev_present(libmtd, i))
			continue;
		err = mtd_get_dev_info1(libmtd, i, &flash->mtd_info[i].mtd);
		if (err) {
			TRACE("No information from MTD%d", i);
			continue;
		}
	}

#if defined(CONFIG_UBIVOL)
	/*
	 * Now search for MTD that are already attached
	 */
	scan_for_ubi_devices();

#if defined(CONFIG_UBIATTACH)
	/*
	 * Search for volumes in MTD that are not attached, default case
	 */

	for (i = mtd_info->lowest_mtd_num;
	     i <= mtd_info->highest_mtd_num; i++) {
		if (flash->libubi && !flash->mtd_info[i].skipubi &&
				!flash->mtd_info[i].scanned &&
				flash->mtd_info[i].mtd.type != MTD_UBIVOLUME)
			scan_ubi_partitions(i);
	}
#endif
#endif

	return mtd_info->mtd_dev_cnt;
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
