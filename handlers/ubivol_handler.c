/*
 * (C) Copyright 2013
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

#include <sys/types.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <mtd/mtd-user.h>
#include "swupdate.h"
#include "handler.h"
#include "flash.h"
#include "util.h"

void ubi_handler(void);

static struct ubi_part *search_volume(const char *str, struct ubilist *list)
{
	struct ubi_part *vol;

	LIST_FOREACH(vol, list, next) {
		if (strcmp(vol->vol_info.name, str) == 0)
			return vol;
	}
	return NULL;
}

static int update_volume(libubi_t libubi, int fdsw, struct img_type *img,
	struct ubi_vol_info *vol)
{
	long long bytes;
	int fdout;
	char node[64];
	int err;
	unsigned long offset = 0;
	uint32_t checksum = 0;
	char sbuf[128];

	bytes = img->size;

	if (!libubi) {
		ERROR("Request to write into UBI, but no UBI on system");
		return -1;
	}

	if (bytes > vol->rsvd_bytes) {
		ERROR("\"%s\" (size %lld) will not fit volume \"%s\" (size %lld)",
		       img->fname, bytes, img->volname, vol->rsvd_bytes);
		return -1;
	}

	snprintf(node, sizeof(node), "/dev/ubi%d_%d",
		vol->dev_num,
		vol->vol_id);

	err = ubi_probe_node(libubi, node);

	if (err == 1) {
		ERROR("\"%s\" is an UBI device node, not an UBI volume node",
			node);
		return -1;
	}
	if (err < 0) {
		if (errno == ENODEV)
			ERROR("%s is not an UBI volume node", node);
		else
			ERROR("error while probing %s", node);
		return -1;
	}

	fdout = open(node, O_RDWR);
	if (fdout < 0) {
		ERROR("cannot open UBI volume \"%s\"", node);
		return -1;
	}
	err = ubi_update_start(libubi, fdout, bytes);
	if (err) {
		ERROR("cannot start volume \"%s\" update", node);
		return -1;
	}

	snprintf(sbuf, sizeof(sbuf), "Installing image %s into volume %s(%s)",
		img->fname, node, img->volname);
	notify(RUN, RECOVERY_NO_ERROR, sbuf);

	printf("Updating UBI : %s %lld %lu\n",
			img->fname, img->size, offset);
	if (copyfile(fdsw, fdout, img->size, (unsigned long *)&img->offset, 0,
		img->compressed, &checksum) < 0) {
		ERROR("Error copying extracted file");
		err = -1;
	}
	close(fdout);
	return err;
}

static int install_ubivol_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	struct flash_description *flash = get_flash_info();
	struct mtd_info *mtd = &flash->mtd;
	struct mtd_ubi_info *mtd_info;
	struct ubi_part *ubivol;
	int i, ret;

	/* Get the correct information */
	for (i = mtd->lowest_mtd_num; i <= mtd->highest_mtd_num; i++) {
		mtd_info = &flash->mtd_info[i];

		ubivol = search_volume(img->volname,
			&mtd_info->ubi_partitions);
		if (ubivol)
			break;
	}
	if (!ubivol) {
		ERROR("Image %s should be stored in volume "
			"%s, but no volume found",
			img->fname,
				img->volname);
		return -1;
	}
	ret = update_volume(flash->libubi, img->fdin, img,
				&ubivol->vol_info);
	return ret;

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
	mtd_info->req.vid_hdr_offset = 0;
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

static int adjust_volume(struct img_type *cfg,
	void __attribute__ ((__unused__)) *data)
{
	struct flash_description *nandubi = get_flash_info();
	struct ubi_part *ubivol;
	struct ubi_mkvol_request req;
	struct mtd_ubi_info *mtd_info;
	int mtdnum;
	char node[64];
	int err;
	struct flash_description *flash = get_flash_info();

	/*
	 * Partition are adjusted only in one MTD device
	 * Other MTD are not touched
	 */
	mtdnum = get_mtd_from_device(cfg->device);
	if (mtdnum < 0 || !mtd_dev_present(flash->libmtd, mtdnum)) {
		ERROR("%s does not exist: partitioning not possible",
			cfg->device);
		return -ENODEV;
	}

	mtd_info = &nandubi->mtd_info[mtdnum];

	/*
	 * First remove all volumes but "data"
	 */
	ubivol = mtd_info->ubi_partitions.lh_first;
	for(ubivol = mtd_info->ubi_partitions.lh_first;
		ubivol != NULL;
		ubivol = ubivol->next.le_next) {
		/* Do not drop data partition */
		if (strcmp(ubivol->vol_info.name, cfg->volname) == 0) {
			break;
		}
	}

	if (ubivol) {
		/* Check if size is changed */
		if (ubivol->vol_info.data_bytes == cfg->partsize)
			return 0;

		snprintf(node, sizeof(node), "/dev/ubi%d", ubivol->vol_info.dev_num);
		err = ubi_rmvol(nandubi->libubi, node, ubivol->vol_info.vol_id);
		if (err) {
			ERROR("Volume %s cannot be dropped", ubivol->vol_info.name);
			return -1;
		}
		TRACE("Removed UBI Volume %s\n", ubivol->vol_info.name);

		LIST_REMOVE(ubivol, next);
		free(ubivol);
	}

	/* We do not need a volume to get the right node */
	snprintf(node, sizeof(node), "/dev/ubi%d", mtd_info->dev_info.dev_num);

	/*
	 * Creates all other partitions as specified in the description file
	 * Volumes are empty, and they are filled later by the update procedure
	 */
	memset(&req, 0, sizeof(req));
	req.vol_type = UBI_DYNAMIC_VOLUME;
	req.vol_id = UBI_VOL_NUM_AUTO;
	req.alignment = 1;
	req.bytes = cfg->partsize;
	req.name = cfg->volname;
	err = ubi_mkvol(nandubi->libubi, node, &req);
	if (err < 0) {
		ERROR("cannot create UBIvolume %s of %lld bytes",
			req.name, req.bytes);
		return err;
	}

	ubivol = (struct ubi_part *)calloc(1, sizeof(struct ubi_part));
	if (!ubivol) {
		ERROR("No memory: malloc failed\n");
		return -ENOMEM;
	}
	err = ubi_get_vol_info1(nandubi->libubi,
		mtd_info->dev_info.dev_num, req.vol_id,
		&ubivol->vol_info);
	if (err) {
		ERROR("cannot get information about "
			"newly created UBI volume");
		return err;
	}
	LIST_INSERT_HEAD(&mtd_info->ubi_partitions, ubivol, next);
	TRACE("Created UBI Volume %s of %lld bytes\n",
		req.name, req.bytes);

	return 0;
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

__attribute__((constructor))
void ubi_handler(void)
{
	register_handler("ubivol", install_ubivol_image, NULL);
	register_handler("ubipartition", adjust_volume, NULL);
}
