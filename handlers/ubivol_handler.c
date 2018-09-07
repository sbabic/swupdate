/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <sys/types.h>
#include <stdio.h>
#include <sys/stat.h>
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
#include "sslapi.h"

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

static int update_volume(libubi_t libubi, struct img_type *img,
	struct ubi_vol_info *vol)
{
	long long bytes;
	int fdout;
	char node[64];
	int err;
	char sbuf[128];
	char *decrypted_size_str = NULL;

	bytes = img->size;
	if (img->is_encrypted) {

		decrypted_size_str = dict_get_value(&img->properties, "decrypted-size");

		bytes = ustrtoull(decrypted_size_str, 0);
		if (errno){
			ERROR("decrypted-size argument: ustrtoull failed");
			return -1;
		}

		if (img->compressed) {
			ERROR("Decryption of compressed UBI images not supported");
			return -1;
		}
		if (bytes < AES_BLOCK_SIZE) {
			ERROR("Encrypted image size (%lld) too small", bytes);
			return -1;
		}
		TRACE("Image is crypted, decrypted size %lld bytes", bytes);
	}

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
	notify(RUN, RECOVERY_NO_ERROR, INFOLEVEL, sbuf);

	TRACE("Updating UBI : %s %lld",
			img->fname, img->size);
	if (copyimage(&fdout, img, NULL) < 0) {
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
	ret = update_volume(flash->libubi, img,
				&ubivol->vol_info);
	return ret;

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
	if (mtdnum < 0) {
		/* Allow device to be specified by name OR number */
		mtdnum = get_mtd_from_name(cfg->device);
	}
	if (mtdnum < 0 || !mtd_dev_present(flash->libmtd, mtdnum)) {
		ERROR("%s does not exist: partitioning not possible",
			cfg->device);
		return -ENODEV;
	}

	mtd_info = &nandubi->mtd_info[mtdnum];

	/*
	 * Search for volume with the same name
	 */
	ubivol = mtd_info->ubi_partitions.lh_first;
	for(ubivol = mtd_info->ubi_partitions.lh_first;
		ubivol != NULL;
		ubivol = ubivol->next.le_next) {
		if (strcmp(ubivol->vol_info.name, cfg->volname) == 0) {
			break;
		}
	}

	if (ubivol) {
		unsigned int requested_lebs, allocated_lebs;

		/* This should never happen, the fields are filled by scan_ubi */
		if (!mtd_info->dev_info.leb_size) {
			return -EFAULT;
		}

		/* Check if size is changed */
		requested_lebs = cfg->partsize / mtd_info->dev_info.leb_size +
			((cfg->partsize % mtd_info->dev_info.leb_size) ? 1 : 0);
		allocated_lebs = ubivol->vol_info.data_bytes / mtd_info->dev_info.leb_size;

		if (requested_lebs == allocated_lebs)
			return 0;

		snprintf(node, sizeof(node), "/dev/ubi%d", ubivol->vol_info.dev_num);
		err = ubi_rmvol(nandubi->libubi, node, ubivol->vol_info.vol_id);
		if (err) {
			ERROR("Volume %s cannot be dropped", ubivol->vol_info.name);
			return -1;
		}
		TRACE("Removed UBI Volume %s", ubivol->vol_info.name);

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
	if (!strcmp(cfg->type_data, "static"))
		req.vol_type = UBI_STATIC_VOLUME;
	else
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
		ERROR("No memory: malloc failed");
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
	TRACE("Created UBI Volume %s of %lld bytes (requested %lld)",
		req.name, ubivol->vol_info.data_bytes, req.bytes);

	return 0;
}

__attribute__((constructor))
void ubi_handler(void)
{
	register_handler("ubivol", install_ubivol_image,
				IMAGE_HANDLER, NULL);
	register_handler("ubipartition", adjust_volume,
				PARTITION_HANDLER, NULL);
}
