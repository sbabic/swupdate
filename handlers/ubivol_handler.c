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
#include "ubi_partition.h"
#include "util.h"

void ubi_handler(void);

struct ubi_part *search_volume(const char *str, struct ubilist *list)
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

int scan_mtd_devices (void)
{
	int err;
	struct flash_description *flash = get_flash_info();
	struct mtd_info *mtd_info = &flash->mtd;
	libmtd_t libmtd = flash->libmtd;
	int i;

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

	for (i = mtd_info->lowest_mtd_num;
	     i <= mtd_info->highest_mtd_num; i++) {
		if (!mtd_dev_present(libmtd, i))
			continue;
		err = mtd_get_dev_info1(libmtd, i, &flash->mtd_info[i].mtd);
		if (err) {
			TRACE("No information from MTD%d", i);
		}

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

static int adjust_partitions(struct img_type *cfg,
	void __attribute__ ((__unused__)) *data)
{
	struct flash_description *nandubi = get_flash_info();
	struct img_type *part;
	struct ubi_part *ubivol, *nextitem, *datavol = NULL;
	struct ubi_mkvol_request req;
	struct mtd_ubi_info *mtd_info;
	struct ubi_rnvol_req rnvol;
	struct ubi_vol_info vol;
	char node[64];
	int err;


	/* Check if a new partition table is required */
	if (!cfg)
		return -1;

	/*
	 * Partition are adjusted only in one MTD device
	 * Other MTD are not touched
	 */
	mtd_info = &nandubi->mtd_info[MTD_FS_DEVICE];

	/*
	 * First remove all volumes but "data"
	 */
	ubivol = mtd_info->ubi_partitions.lh_first;
	while (ubivol != NULL) {
		/* Do not drop data partition */
		if (strcmp(ubivol->vol_info.name, UBI_DATA_VOLNAME) == 0) {
			datavol = ubivol;
			ubivol = ubivol->next.le_next;
			continue;
		}

		snprintf(node, sizeof(node), "/dev/ubi%d", ubivol->vol_info.dev_num);
		err = ubi_rmvol(nandubi->libubi, node, ubivol->vol_info.vol_id);
		if (err) {
			ERROR("Volume %s cannot be dropped", ubivol->vol_info.name);
			exit(1);
		}
		TRACE("Removed UBI Volume %s\n", ubivol->vol_info.name);

		nextitem = LIST_NEXT(ubivol, next);
		LIST_REMOVE(ubivol, next);
		free(ubivol);
		ubivol = nextitem;
	}

	/* We do not need a volume to get the right node */
	snprintf(node, sizeof(node), "/dev/ubi%d", mtd_info->dev_info.dev_num);

	/* Check if DATA partition must be adjusted */
	for ( part = cfg; part != NULL; part = part->next.le_next) {
		if (strcmp(part->volname, UBI_DATA_VOLNAME) == 0) {
			break;
		}
	}

	/*
	 * Saves the data volumes adjusting the partition size.
	 * Because the partition can be shrinked, due to limitations
	 * changing UBIFS size the following procedure is implemented:
	 *
	 * - creates a volume with the size specified in the description file
	 *   this volume assumes the name "datacpy"
	 * - mount both "data" and "datacpy"
	 * - copy all files from "data" to "datacpy"
	 * - rename both volumes in one shot exchanging their names
	 *   "datacpy" becomes "data", "data" becomes "datacpy"
	 * - drop volume "datacpy" (partition with old size)
	 */
	if ( (part != NULL) && (part->size != datavol->vol_info.data_bytes) ) {
		TRACE("New \"data\" size : from %lld to %lld bytes\n",
			datavol->vol_info.data_bytes,
			part->size);

		/* A new size is requested, create a new vol */
		memset(&req, 0, sizeof(req));
		req.vol_type = UBI_DYNAMIC_VOLUME;
		req.vol_id = UBI_VOL_NUM_AUTO;
		req.alignment = 1;

		req.bytes = part->size;
		req.name = UBI_DATACPY_VOLNAME;
		err = ubi_mkvol(nandubi->libubi, node, &req);
		if (err < 0)
			ERROR("cannot create UBIvolume %s of %lld bytes",
				req.name, req.bytes);

		/* we have now two volumes, copy all data from the old one */
		err = ubi_get_vol_info1(nandubi->libubi,
			mtd_info->dev_info.dev_num, req.vol_id,
			&vol);
		if (err)
			ERROR("cannot get information about newly created UBI volume");
		ubi_mount(&datavol->vol_info, DATASRC_DIR);
		ubi_mount(&vol, DATADST_DIR);
		if (!isDirectoryEmpty(DATASRC_DIR)) {
			err = system("cp -r " DATASRC_DIR "/* " DATADST_DIR);
			if (err)
				ERROR("DATA cannot be copied : %s",
					strerror(errno));
		} else {
			TRACE("\"data\" is empty, skipping copy...\n");
		}

		/* Now switch the two copies, the new one becomes active */
		ubi_umount(DATASRC_DIR);
		ubi_umount(DATADST_DIR);
		rnvol.ents[0].vol_id = datavol->vol_info.vol_id;
		rnvol.ents[0].name_len = strlen(UBI_DATACPY_VOLNAME);
		strcpy(rnvol.ents[0].name, UBI_DATACPY_VOLNAME);
		rnvol.ents[1].vol_id = vol.vol_id;
		rnvol.ents[1].name_len = strlen(UBI_DATA_VOLNAME);
		strcpy(rnvol.ents[1].name, UBI_DATA_VOLNAME);
		rnvol.count = 2;
		err = ubi_rnvols(nandubi->libubi, node, &rnvol);
		if (err) {
			ERROR("Cannot rename partitions %s %s : %s",
				UBI_DATACPY_VOLNAME, UBI_DATA_VOLNAME,
				strerror(errno));
			return err;
		}

		/* Drop the old partition */
		strncpy(vol.name, UBI_DATA_VOLNAME, UBI_VOL_NAME_MAX);
		err = ubi_rmvol(nandubi->libubi, node, datavol->vol_info.vol_id);
		if (err) {
			ERROR("Volume id %d(%s) cannot be dropped",
				datavol->vol_info.vol_id,
				UBI_DATACPY_VOLNAME);
			return err;
		}


		/* Update ubi_vol_info for the "data" partition */
		memcpy(&datavol->vol_info, &vol, sizeof(datavol->vol_info));
	}

	/*
	 * Creates all other partitions as specified in the description file
	 * Volumes are empty, and they are filled later by the update procedure
	 */
	for (part = cfg ; part != NULL;
		part = part->next.le_next) {
		if (strcmp(part->volname, UBI_DATA_VOLNAME) == 0) {
			continue;
		}
		memset(&req, 0, sizeof(req));
		req.vol_type = UBI_DYNAMIC_VOLUME;
		req.vol_id = UBI_VOL_NUM_AUTO;
		req.alignment = 1;
		req.bytes = part->size;
		req.name = part->volname;
		err = ubi_mkvol(nandubi->libubi, node, &req);
		if (err < 0) {
			ERROR("cannot create UBIvolume %s of %lld bytes",
				req.name, req.bytes);
			return err;
		}

		ubivol = (struct ubi_part *)calloc(1, sizeof(struct ubi_part));
		if (!ubivol) {
			ERROR("No memory: malloc failed\n");
			return err;
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
		TRACE("Created Volume %s of %lld bytes\n",
			req.name, req.bytes);
	}

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
	register_handler("ubipartition", adjust_partitions, NULL);
}
