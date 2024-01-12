/*
 * (C) Copyright 2013
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
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
#include "swupdate_image.h"
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

/* search a UBI volume by name across all mtd partitions */
static struct ubi_part *search_volume_global(const char *str)
{
	struct flash_description *flash = get_flash_info();
	struct mtd_info *mtd_info = &flash->mtd;
	struct mtd_ubi_info *mtd_ubi_info;
	struct ubi_part *ubivol;
	int i;

	for (i = mtd_info->lowest_mtd_num; i <= mtd_info->highest_mtd_num; i++) {
		mtd_ubi_info = &flash->mtd_info[i];
		ubivol = search_volume(str, &mtd_ubi_info->ubi_partitions);
		if (ubivol)
			return ubivol;
	}
	return NULL;
}

/**
 * check_replace - check for and validate replace property
 * @img: image information
 * @vol1: pointer to install target volume
 * @vol2: pointer to ubi_vol_info pointer. Will be set to point to the
 *	  to-be-replaced volume in case a volume is found and the
 *	  request is legal. Otherwise it is set to NULL.
 * @rename: pointer to new target volume name pointer.
 *
 * Return: 0 if replace valid or no replace found. Otherwise <0.
 */
static int check_replace(struct img_type *img,
			 struct ubi_vol_info *vol1,
			 struct ubi_vol_info **vol2,
			 char **rename)
{
	char *tmpvol_name;
	struct ubi_part *tmpvol;

	*vol2 = NULL;
	*rename = NULL;
	tmpvol_name = dict_get_value(&img->properties, "replaces");

	if (tmpvol_name == NULL)
		return 0;

	tmpvol = search_volume_global(tmpvol_name);

	if (!tmpvol) {
		INFO("replace: unable to find a volume %s, will rename", tmpvol_name);
		*rename = tmpvol_name;
		return 0;
	}

	/* check whether on same device */
	if (vol1->dev_num != tmpvol->vol_info.dev_num) {
		ERROR("replace: unable to swap volumes on different devices");
		return -1;
	}

	TRACE("replace: will swap UBI volume names %s <-> %s after successful install",
	      vol1->name, tmpvol->vol_info.name);

	*vol2 = &tmpvol->vol_info;

	return 0;
}

/**
 * swap_volnames - swap the names of the given volumes
 * @vol1: first volume
 * @vol2: second volume
 *
 * Return: 0 if OK, <0 otherwise
 */
static int swap_volnames(libubi_t libubi,
			 struct ubi_vol_info *vol1,
			 struct ubi_vol_info *vol2)
{
	struct ubi_rnvol_req rnvol;
	char masternode[64];

	snprintf(masternode, sizeof(masternode),
		 "/dev/ubi%d", vol1->dev_num);

	TRACE("replace: swapping UBI volume names %s <-> %s on %s",
	      vol1->name, vol2->name, masternode);

	rnvol.ents[0].vol_id = vol1->vol_id;
	rnvol.ents[0].name_len = strlen(vol2->name);
	strcpy(rnvol.ents[0].name, vol2->name);

	rnvol.ents[1].vol_id = vol2->vol_id;
	rnvol.ents[1].name_len = strlen(vol1->name);
	strcpy(rnvol.ents[1].name, vol1->name);

	rnvol.count = 2;

	return ubi_rnvols(libubi, masternode, &rnvol);
}

/**
 * rename_vol - rename the given volume
 * @vol: volume
 * @name: new volume name
 *
 * Return: 0 if OK, <0 otherwise
 */
static int rename_vol(libubi_t libubi,
			 struct ubi_vol_info *vol,
			 char *name)
{
	struct ubi_rnvol_req rnvol;
	char masternode[64];

	snprintf(masternode, sizeof(masternode),
		 "/dev/ubi%d", vol->dev_num);

	TRACE("replace: rename UBI volume %s to %s on %s",
	      vol->name, name, masternode);

	rnvol.ents[0].vol_id = vol->vol_id;
	rnvol.ents[0].name_len = strlen(name);
	strlcpy(rnvol.ents[0].name, name, sizeof(rnvol.ents[0].name));
	rnvol.count = 1;

	return ubi_rnvols(libubi, masternode, &rnvol);
}

/**
 * check_ubi_alwaysremove - check the property always-remove for this image
 * @img: image information
 *
 * Return: 1 if the property always-remove is true, otherwise 0.
 */
static bool check_ubi_alwaysremove(struct img_type *img)
{
	return strtobool(dict_get_value(&img->properties, "always-remove"));
}

static int update_volume(libubi_t libubi, struct img_type *img,
	struct ubi_vol_info *vol)
{
	long long bytes;
	int fdout;
	char node[64];
	int err;
	char sbuf[128];
	char *rn_vol;
	struct ubi_vol_info *repl_vol;

	bytes = get_output_size(img, true);
	if (bytes <= 0)
		return -1;

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

	/* check replace property */
	if(check_replace(img, vol, &repl_vol, &rn_vol))
		return -1;

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
			img->fname, bytes);
	if (copyimage(&fdout, img, NULL) < 0) {
		ERROR("Error copying extracted file");
		err = -1;
	}

	/* handle replace */
	if(repl_vol) {
		err = swap_volnames(libubi, vol, repl_vol);
		if(err)
			ERROR("replace: failed to swap volume names %s<->%s: %d",
			      vol->name, repl_vol->name, err);
	} else if (rn_vol) {
		err = rename_vol(libubi, vol, rn_vol);
		if(err)
			ERROR("replace: failed to rename %s to %s: %d",
			      vol->name, rn_vol, err);
	}

	close(fdout);
	return err;
}

static int resize_volume(struct img_type *cfg, long long size)
{
	struct flash_description *nandubi = get_flash_info();
	struct ubi_part *ubivol;
	struct ubi_mkvol_request req;
	struct mtd_ubi_info *mtd_info;
	int mtdnum, req_vol_type;
	char node[64];
	int err;
	struct flash_description *flash = get_flash_info();

	/* determine the requested volume type */
	if (!strcmp(cfg->type_data, "static"))
		req_vol_type = UBI_STATIC_VOLUME;
	else
		req_vol_type = UBI_DYNAMIC_VOLUME;

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
		requested_lebs = size / mtd_info->dev_info.leb_size +
			((size % mtd_info->dev_info.leb_size) ? 1 : 0);
		allocated_lebs = ubivol->vol_info.rsvd_bytes / mtd_info->dev_info.leb_size;

		if (requested_lebs == allocated_lebs &&
		    req_vol_type == ubivol->vol_info.type &&
		    !check_ubi_alwaysremove(cfg)) {
			TRACE("skipping volume %s (same size and type)",
			      ubivol->vol_info.name);
			return 0;
		}

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

	if (size) {
		/* We do not need a volume to get the right node */
		snprintf(node, sizeof(node), "/dev/ubi%d", mtd_info->dev_info.dev_num);

		/*
		 * Creates all other partitions as specified in the description file
		 * Volumes are empty, and they are filled later by the update procedure
		 */
		memset(&req, 0, sizeof(req));
		req.vol_type = req_vol_type;
		req.vol_id = UBI_VOL_NUM_AUTO;
		req.alignment = 1;
		req.bytes = size;
		req.name = cfg->volname;
		err = ubi_mkvol(nandubi->libubi, node, &req);
		if (err < 0) {
			ERROR("cannot create %s UBI volume %s of %lld bytes",
				  (req_vol_type == UBI_DYNAMIC_VOLUME) ? "dynamic" : "static",
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
		TRACE("Created %s UBI volume %s of %lld bytes (old size %lld)",
			  (req_vol_type == UBI_DYNAMIC_VOLUME) ? "dynamic" : "static",
			  req.name, req.bytes, ubivol->vol_info.rsvd_bytes);
	}

	return 0;
}

/**
 * check_auto_resize - check the property auto-resize for this image
 * @img: image information
 *
 * Return: 1 if the property auto-resize is true, otherwise 0.
 */
static bool check_ubi_autoresize(struct img_type *img)
{
	return strtobool(dict_get_value(&img->properties, "auto-resize"));
}

static int wait_volume(struct img_type *img)
{
	int ret = -1, num = 0, dev_num, vol_id;
	struct ubi_part *ubivol;
	struct stat buf;
	char node[64];

	ubivol = search_volume_global(img->volname);
	if (!ubivol) {
		ERROR("can't found volume %s", img->volname);
		return -1;
	}

	dev_num = ubivol->vol_info.dev_num;
	vol_id  = ubivol->vol_info.vol_id;

	snprintf(node, sizeof(node), "/dev/ubi%d_%d",
		 dev_num,
		 vol_id);

	while (num++ < 5)
	{
		ret = stat(node, &buf);
		if (!ret)
			break;

		sleep(1);
	}

	return ret;
}

static int install_ubivol_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	struct flash_description *flash = get_flash_info();
	struct ubi_part *ubivol;
	int ret;

	if (check_ubi_autoresize(img)) {
		long long bytes = get_output_size(img, true);
		if (bytes <= 0)
			return -1;

		ret = resize_volume(img, bytes);
		if (ret < 0) {
			ERROR("Can't resize ubi volume %s", img->volname);
			return -1;
		}

		ret = wait_volume(img);
		if (ret < 0) {
			ERROR("can't found ubi volume %s", img->volname);
			return -1;
		}
	}

	/* find the volume to be updated */
	ubivol = search_volume_global(img->volname);

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
	return resize_volume(cfg, cfg->partsize);
}

static int ubi_volume_get_info(char *name, int *dev_num, int *vol_id)
{
	struct ubi_part *ubi_part;

	ubi_part = search_volume_global(name);
	if (!ubi_part) {
		ERROR("could not found UBI volume %s", name);
		return -1;
	}

	*dev_num = ubi_part->vol_info.dev_num;
	*vol_id  = ubi_part->vol_info.vol_id;

	return 0;
}

static int swap_volume(struct img_type *img, void *data)
{
	struct script_handler_data *script_data;
	struct flash_description *flash = get_flash_info();
	libubi_t libubi = flash->libubi;
	int num, count = 0;
	struct dict_list *volumes;
	struct dict_list_elem *volume;
	char *name[2];
	int dev_num[2], vol_id[2], global_dev_num = -1;
	char prop[SWUPDATE_GENERAL_STRING_SIZE];
	char masternode[UBI_MAX_VOLUME_NAME+1];
	struct ubi_rnvol_req rnvol;
	int ret = -1;

	if (!data)
		return -EINVAL;

	script_data = data;

	/*
	 * Call only in case of postinstall
	 */
	if (script_data->scriptfn != POSTINSTALL)
		return 0;

	while (1) {
		snprintf(prop, sizeof(prop), "swap-%d", count);
		volumes = dict_get_list(&img->properties, prop);
		if (!volumes)
			break;

		if (count >= (UBI_MAX_RNVOL / 2)) {
			ERROR("Too many requested swap");
			goto out;
		}

		num = 0;
		LIST_FOREACH(volume, volumes, next) {
			if (num >= 2) {
				ERROR("Too many ubi volume (%s)", prop);
				goto out;
			}

			name[num] = volume->value;
			if (ubi_volume_get_info(volume->value,
						&dev_num[num],
						&vol_id[num]) < 0)
				goto out;

			num++;
		}

		if (num != 2) {
			ERROR("Invalid number (%d) of ubi volume (%s)", num, prop);
			goto out;
		}

		if (dev_num[0] != dev_num[1]) {
			ERROR("both volume should be on the same UBI device");
			goto out;
		}

		if (global_dev_num == -1) {
			global_dev_num = dev_num[0];
			snprintf(&masternode[0], sizeof(masternode),
				 "/dev/ubi%d", global_dev_num);
		} else {
			if (global_dev_num != dev_num[0]) {
				ERROR("all volumes should be on the"
				      "same UBI device (%s)", prop);
				goto out;
			}
		}

		TRACE("swap UBI volume %s <-> %s", name[0], name[1]);

		/* swap first -> second */
		rnvol.ents[2 * count + 0].vol_id = vol_id[0];
		rnvol.ents[2 * count + 0].name_len = min(strlen(name[1]), UBI_MAX_VOLUME_NAME);
		strlcpy(rnvol.ents[2 * count + 0].name, name[1], UBI_MAX_VOLUME_NAME);

		/* swap second -> first */
		rnvol.ents[2 * count + 1].vol_id = vol_id[1];
		rnvol.ents[2 * count + 1].name_len = min(strlen(name[0]), UBI_MAX_VOLUME_NAME);
		strlcpy(rnvol.ents[2 * count + 1].name, name[0], UBI_MAX_VOLUME_NAME);

		count++;
	}

	if (!count) {
		ERROR("No UBI volume provided");
		goto out;
	}

	rnvol.count = count * 2;

	ret = ubi_rnvols(libubi, masternode, &rnvol);
	if (ret)
		ERROR("failed to swap UBI volume names");

 out:
	return ret;
}

__attribute__((constructor))
void ubi_handler(void)
{
	register_handler("ubivol", install_ubivol_image,
				IMAGE_HANDLER, NULL);
	register_handler("ubipartition", adjust_volume,
				PARTITION_HANDLER | NO_DATA_HANDLER, NULL);
	register_handler("ubiswap", swap_volume,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}
