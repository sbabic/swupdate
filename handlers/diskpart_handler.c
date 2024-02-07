/*
 * (C) Copyright 2019
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <libfdisk/libfdisk.h>
#include <linux/fs.h>
#include <fs_interface.h>
#include <uuid/uuid.h>
#include <dirent.h>
#include <libgen.h>
#include "swupdate_image.h"
#include "handler.h"
#include "util.h"
#include "progress.h"

void diskpart_handler(void);
void diskpart_toggle_boot(void);
void diskpart_gpt_swap_partition(void);
void diskpart_install_gpt_partition_image(void);

/*
 * This is taken from libfdisk to declare if a field is not set
 */
#define LIBFDISK_INIT_UNDEF(_x)	((__typeof__(_x)) -1)

/* Linux native partition type */
 #define GPT_DEFAULT_ENTRY_TYPE "0FC63DAF-8483-4772-8E79-3D69D8477DE4"

/*
 * We will only have a parent in hybrid mode.
 */
#define IS_HYBRID(cxt) fdisk_get_parent(cxt)

/*
 * Get the parent if it exists, otherwise context is already the parent.
 */
#define PARENT(cxt) fdisk_get_parent(cxt) ? fdisk_get_parent(cxt) : cxt

/**
 * Keys for the properties field in sw-description
 */
enum partfield {
	PART_SIZE = 0,
	PART_START,
	PART_TYPE,
	PART_NAME,
	PART_FSTYPE,
	PART_DOSTYPE,
	PART_UUID,
	PART_FLAG,
	PART_FORCE
};

const char *fields[] = {
	[PART_SIZE] = "size",
	[PART_START] = "start",
	[PART_TYPE] = "type",
	[PART_NAME] = "name",
	[PART_FSTYPE] = "fstype",
	[PART_DOSTYPE] = "dostype",
	[PART_UUID] = "partuuid",
	[PART_FLAG] = "flag",
	[PART_FORCE] = "force",
};

struct partition_data {
	size_t partno;
	uint64_t size;
	size_t start;
	char type[SWUPDATE_GENERAL_STRING_SIZE];
	char name[SWUPDATE_GENERAL_STRING_SIZE];
	char fstype[SWUPDATE_GENERAL_STRING_SIZE];
	char dostype[SWUPDATE_GENERAL_STRING_SIZE];
	char partuuid[UUID_STR_LEN];
	int explicit_size;
	unsigned long flags;
	int force;
	LIST_ENTRY(partition_data) next;
};
LIST_HEAD(listparts, partition_data);

/*
 * Internal handler data
 */
struct hnd_priv {
	enum fdisk_labeltype labeltype;
	bool nolock;
	bool noinuse;
	struct listparts listparts;	/* list of partitions */
};

struct create_table {
	bool parent;
	bool child;
};

struct diskpart_table {
	struct fdisk_table *parent;
	struct fdisk_table *child;
};

static char *diskpart_get_lbtype(struct img_type *img)
{
	return dict_get_value(&img->properties, "labeltype");
}

static bool diskpart_is_gpt(struct img_type *img)
{
	char *lbtype = diskpart_get_lbtype(img);
	return (lbtype && !strcmp(lbtype, "gpt"));
}

static bool diskpart_is_dos(struct img_type *img)
{
	char *lbtype = diskpart_get_lbtype(img);
	return (lbtype && !strcmp(lbtype, "dos"));
}

static int diskpart_assign_label(struct fdisk_context *cxt, struct img_type *img,
		struct hnd_priv priv, struct create_table *createtable, unsigned long hybrid)
{
	char *lbtype = diskpart_get_lbtype(img);
	int ret = 0;

	/*
	 * Check partition table
	 */
	if (!fdisk_has_label(cxt)) {
		WARN("%s does not contain a recognized partition table",
			 img->device);
		ret = fdisk_create_disklabel(cxt, lbtype);
		if (ret) {
			ERROR("Failed to create disk label");
			return ret;
		}
		createtable->parent = true;
		if (hybrid)
			createtable->child = true;
	} else if (lbtype) {
		if (diskpart_is_gpt(img)) {
			priv.labeltype = FDISK_DISKLABEL_GPT;
		} else {
			priv.labeltype = FDISK_DISKLABEL_DOS;
		}

		if (!fdisk_is_labeltype(cxt, priv.labeltype)) {
			WARN("Partition table of different type, setting to %s, all data lost !",
				 lbtype);
			ret = fdisk_create_disklabel(cxt, lbtype);
			if (ret) {
				ERROR("Failed to create disk label");
				return ret;
			}
			createtable->parent = true;
			if (hybrid)
				createtable->child = true;
		}
	}

	return ret;
}

static int diskpart_assign_context(struct fdisk_context **cxt,struct img_type *img,
		struct hnd_priv priv, unsigned long hybrid, struct create_table *createtable)
{
	struct fdisk_context *parent;
	char *path = NULL;
	int ret = 0;

	/*
	 * Parent context, accessed through the child context when
	 * used in hybrid mode.
	 */
	parent = fdisk_new_context();
	if (!parent) {
		ERROR("Failed to allocate libfdisk context");
		return -ENOMEM;
	}

	/*
	 * The library uses dialog driven partitioning by default.
	 * Disable as we don't support interactive dialogs.
	 */
	ret = fdisk_disable_dialogs(parent, 1);
	if (ret) {
		ERROR("Failed to disable dialogs");
		return ret;
	}

	/*
	 * Resolve device path symlink.
	 */
	path = realpath(img->device, NULL);
	if (!path)
		path = strdup(img->device);

	/*
	 * fdisk_new_nested_context requires the device to be assigned.
	 */
	ret = fdisk_assign_device(parent, path, 0);
	free(path);
	if (ret < 0) {
		ERROR("Device %s cannot be opened: %s", img->device, strerror(-ret));
		return ret;
	}

	/*
	 * fdisk_new_nested_context requires the parent label to be set.
	 */
	ret = diskpart_assign_label(parent, img, priv, createtable, hybrid);
	if (ret)
		return ret;

	if (hybrid) {
		/*
		 * Child context which we will use for the hybrid dos
		 * table in GPT mode.
		 *
		 * This also lets us access the parent context.
		 */
		*cxt = fdisk_new_nested_context(parent, "dos");
		if (!*cxt) {
			ERROR("Failed to allocate libfdisk nested context");
			return -ENOMEM;
		}

		/*
		 * The library uses dialog driven partitioning by default.
		 * Disable as we don't support interactive dialogs.
		 */
		ret = fdisk_disable_dialogs(*cxt, 1);
		if (ret) {
			ERROR("Failed to disable nested dialogs");
			return ret;
		}
	} else {
		/*
		 * Use the parent context directly when not in hybrid mode.
		 */
		*cxt = parent;
	}

	return ret;
}

static struct diskpart_table *diskpart_new_table(struct fdisk_context *cxt)
{
	struct diskpart_table *tb = NULL;

	tb = calloc(1, sizeof(*tb));
	if (!tb)
		return NULL;

	tb->parent = fdisk_new_table();
	if (!tb->parent) {
		free(tb);
		return NULL;
	}

	if (IS_HYBRID(cxt)) {
		tb->child = fdisk_new_table();
		if (!tb->child) {
			fdisk_unref_table(tb->parent);
			free(tb);
			return NULL;
		}
	}

	return tb;
}

static void diskpart_unref_table(struct diskpart_table *tb)
{
	if (!tb)
		return;

	if (tb->child)
		fdisk_unref_table(tb->child);

	if (tb->parent)
		fdisk_unref_table(tb->parent);

	free(tb);
}

static int diskpart_get_partitions(struct fdisk_context *cxt, struct diskpart_table *tb,
		struct create_table *createtable)
{
	int ret = 0;

	if (fdisk_get_partitions(PARENT(cxt), &tb->parent))
		createtable->parent = true;

	if (IS_HYBRID(cxt) && fdisk_get_partitions(cxt, &tb->child))
		createtable->child = true;

	return ret;
}

static struct fdisk_partition *
diskpart_fdisk_table_get_partition_by_name(struct fdisk_table *tb, char *name)
{
	struct fdisk_partition *pa = NULL;
	struct fdisk_partition *ipa = NULL;
	struct fdisk_iter *itr;
	const char *iname;

	if (!tb || !name)
		goto out;

	itr = fdisk_new_iter(FDISK_ITER_FORWARD);

	while (!fdisk_table_next_partition(tb, itr, &ipa)) {
		iname = fdisk_partition_get_name(ipa);
		if (iname && !strcmp(iname, name)) {
			pa = ipa;
			break;
		}
	}

	fdisk_free_iter(itr);

 out:
	return pa;
}

static struct fdisk_partition *
diskpart_get_partition_by_name(struct diskpart_table *tb, char *name)
{
	struct fdisk_partition *pa = NULL;

	if (!tb || !name)
		goto out;

	if (tb->parent)
		pa = diskpart_fdisk_table_get_partition_by_name(tb->parent, name);

 out:
	return pa;
}

static int diskpart_swap_partition(struct diskpart_table *tb,
				   struct create_table *createtable,
				   char *name1, char *name2)
{
	struct fdisk_partition *pa1 = NULL, *pa2 = NULL;
	int ret = -1;

	pa1 = diskpart_get_partition_by_name(tb, name1);
	if (!pa1) {
		ERROR("Can't find partition %s", name1);
		goto out;
	}

	pa2 = diskpart_get_partition_by_name(tb, name2);
	if (!pa2) {
		ERROR("Can't find partition %s", name2);
		goto out;
	}

	ret = fdisk_partition_set_name(pa1, name2);
	if (ret)
		goto out;
	ret = fdisk_partition_set_name(pa2, name1);
	if (ret)
		goto out;

	createtable->parent = true;

 out:
	return ret;
}

static int diskpart_set_partition(struct fdisk_partition *pa,
				  struct partition_data *part,
				  unsigned long sector_size,
				  struct fdisk_parttype *parttype,
				  struct fdisk_table *oldtb)
{
	int ret;

	fdisk_partition_unset_partno(pa);
	fdisk_partition_unset_size(pa);
	fdisk_partition_unset_start(pa);
	if (part->start != LIBFDISK_INIT_UNDEF(part->start))
		ret = fdisk_partition_set_start(pa, part->start);
	else
		ret = fdisk_partition_start_follow_default(pa, 1);
	if (part->partno != LIBFDISK_INIT_UNDEF(part->partno))
		ret |= fdisk_partition_set_partno(pa, part->partno);
	else
		ret |= -EINVAL;
	if (strlen(part->name))
	      ret |= fdisk_partition_set_name(pa, part->name);
	if (part->size != LIBFDISK_INIT_UNDEF(part->size)) {
	      ret |= fdisk_partition_set_size(pa, part->size / sector_size);
	      if (part->explicit_size)
			ret |= fdisk_partition_size_explicit(pa, part->explicit_size);
	} else {
		ret |= fdisk_partition_end_follow_default(pa, 1);
	}

	if (parttype)
		ret |= fdisk_partition_set_type(pa, parttype);

	if (strlen(part->partuuid)) {
		ret |= fdisk_partition_set_uuid(pa, part->partuuid);
	} else {
		/*
		 * If the uuid is not set a random one will be generated, retrieve the
		 * existing uuid from the on-disk partition if one exists so that we
		 * don't mark the partition as changed due to a different random uuid.
		 */
		struct fdisk_partition *oldpart = fdisk_table_get_partition_by_partno(oldtb, part->partno);
		if (oldpart) {
			const char *uuid = fdisk_partition_get_uuid(oldpart);
			if (uuid)
				ret |= fdisk_partition_set_uuid(pa, uuid);
		}
	}

	return ret;
}

static int diskpart_set_hybrid_partition(struct fdisk_partition *pa,
								  struct partition_data *part,
								  struct fdisk_parttype *parttype,
								  struct fdisk_table *tb)
{
	/*
	 * Lookup the parent partition by partition number so that we
	 * can align the nested/hybrid partition entries properly.
	 */
	struct fdisk_partition *parent = fdisk_table_get_partition_by_partno(tb, part->partno);
	int ret = 0;

	if (!parent) {
		ERROR("I cannot find parent for hybrid partition %zu(%s)", part->partno, part->name);
		return -EINVAL;
	};

	fdisk_partition_unset_partno(pa);
	fdisk_partition_unset_size(pa);
	fdisk_partition_unset_start(pa);
	fdisk_partition_size_explicit(pa, 1);
	if (fdisk_partition_has_start(parent))
		ret = fdisk_partition_set_start(pa, fdisk_partition_get_start(parent));
	else
		ret = -EINVAL;
	ret |= fdisk_partition_partno_follow_default(pa, 1);
	if (strlen(part->name))
		ret |= fdisk_partition_set_name(pa, part->name);
	if (fdisk_partition_has_size(parent))
		ret |= fdisk_partition_set_size(pa, fdisk_partition_get_size(parent));
	else
		ret |= -EINVAL;

	if (parttype)
		ret |= fdisk_partition_set_type(pa, parttype);

	return ret;
}

static int diskpart_append_hybrid_pmbr(struct fdisk_label *lb, struct fdisk_table *tb)
{
	struct fdisk_partition *pa;
	struct fdisk_parttype *parttype;
	int ret = 0;

	pa = fdisk_new_partition();
	fdisk_partition_unset_partno(pa);
	fdisk_partition_unset_size(pa);
	fdisk_partition_unset_start(pa);
	fdisk_partition_size_explicit(pa, 1);

	/*
	 * Place the hybrid PMBR over the GPT header
	 */
	ret = fdisk_partition_set_start(pa, 1);
	ret |= fdisk_partition_set_size(pa, 33);

	/*
	 * Set type to 0xEE(Intel EFI GUID Partition Table) for hybrid PMBR
	 */
	parttype = fdisk_label_get_parttype_from_code(lb, 0xee);
	ret |= fdisk_partition_set_type(pa, parttype);

	/*
	 * Just append the hybrid PMBR entry at the end since Linux will
	 * run in GPT mode if any primary DOS entry is 0xEE.
	 */
	ret |= fdisk_partition_partno_follow_default(pa, 1);
	if (ret)
		return ret;

	if ((ret = fdisk_table_add_partition(tb, pa)) < 0) {
		ERROR("Failed to append hybrid PMBR to table");
	}
	fdisk_unref_partition(pa);

	return ret;
}

static void diskpart_partition_info(struct fdisk_context *cxt, const char *name, struct fdisk_partition *pa)
{
	struct fdisk_label *lb;
	int *ids = NULL;
	size_t nids = 0;
	size_t i;
	lb = fdisk_get_label(cxt, NULL);
	fdisk_label_get_fields_ids_all(lb, cxt, &ids, &nids);
	if (ids && lb) {
		TRACE("%s:", name);
		for (i = 0; i < nids; i++) {
			const struct fdisk_field *field =
					fdisk_label_get_field(lb, ids[i]);
			char *data = NULL;
			if (!field)
				continue;
			if (fdisk_partition_to_string(pa, cxt, ids[i], &data))
				continue;
			TRACE("\t%s: %s", fdisk_field_get_name(field), data);
			free(data);
		}
	} else {
		if (!ids)
			ERROR("Failed to load field ids");
		if (!lb)
			ERROR("Failed to load label");
	}
	if (ids)
		free(ids);
}

/*
 * Return true if partition differs
 */
static bool is_diskpart_different(struct fdisk_partition *firstpa, struct fdisk_partition *secondpa)
{
	if (!firstpa || !secondpa)
		return true;

	if (fdisk_partition_cmp_partno(firstpa, secondpa) ||
		(!fdisk_partition_start_is_default(firstpa) && !fdisk_partition_start_is_default(secondpa) &&
			fdisk_partition_cmp_start(firstpa, secondpa)) ||
		fdisk_partition_get_size(firstpa) != fdisk_partition_get_size(secondpa)) {
		return true;
	}

	struct fdisk_parttype *firstpa_type = fdisk_partition_get_type(firstpa);
	if (!firstpa_type)
		return true;
	struct fdisk_parttype *secondpa_type = fdisk_partition_get_type(secondpa);

	if (fdisk_parttype_get_string(firstpa_type)) {
		/* gpt */
		const char *firstpa_name = fdisk_partition_get_name(firstpa);
		const char *secondpa_name = fdisk_partition_get_name(secondpa);
		if ((secondpa_type && strcmp(fdisk_parttype_get_string(firstpa_type), fdisk_parttype_get_string(secondpa_type))) ||
			strcmp(firstpa_name ? firstpa_name : "", secondpa_name ? secondpa_name : "")) {
			return true;
		}

		const char *firstpa_uuid = fdisk_partition_get_uuid(firstpa);
		const char *secondpa_uuid = fdisk_partition_get_uuid(secondpa);
		if (firstpa_uuid && secondpa_uuid && strcmp(firstpa_uuid, secondpa_uuid)) {
			return true;
		}
	} else {
		/* dos */
		if (fdisk_parttype_get_code(firstpa_type) != fdisk_parttype_get_code(secondpa_type)) {
			return true;
		}
		if (fdisk_partition_is_bootable(firstpa) != fdisk_partition_is_bootable(secondpa)) {
			return true;
		}
	}

	return false;
}

static int diskpart_reload_table(struct fdisk_context *cxt, struct fdisk_table *tb)
{
	int ret = 0;

	ret = fdisk_delete_all_partitions(cxt);
	if (ret) {
		ERROR("Partition table cannot be deleted: %d", ret);
		return ret;
	}
	ret = fdisk_apply_table(cxt, tb);
	if (ret) {
		ERROR("Partition table cannot be applied: %d", ret);
		return ret;
	}
	fdisk_reset_table(tb);
	ret = fdisk_get_partitions(cxt, &tb);
	if (ret) {
		ERROR("Error loading applied table %d:", ret);
		return ret;
	}
	return ret;
}

static int diskpart_fill_table(struct fdisk_context *cxt, struct diskpart_table *tb,
		struct diskpart_table *oldtb, struct partition_data *part, struct hnd_priv priv)
{
	struct fdisk_parttype *parttype;
	struct fdisk_label *lb;
	unsigned long sector_size;
	int ret = 0;

	lb = fdisk_get_label(PARENT(cxt), NULL);
	if (!lb) {
		ERROR("Failed to load label");
		return -EINVAL;
	}

	sector_size = fdisk_get_sector_size(PARENT(cxt));
	if (!sector_size)
		sector_size = 1;

	LIST_FOREACH(part, &priv.listparts, next) {
		struct fdisk_partition *newpa;

		newpa = fdisk_new_partition();
		/*
		 * GPT uses strings instead of hex code for partition type
		 */
		if (fdisk_is_label(PARENT(cxt), GPT)) {
			if (part->type[0]) {
				parttype = fdisk_label_get_parttype_from_string(lb, part->type);
				if (!parttype)
					parttype = fdisk_new_unknown_parttype(0, part->type);
			} else {
				parttype = fdisk_label_get_parttype_from_string(lb, GPT_DEFAULT_ENTRY_TYPE);
			}
		} else {
			parttype = fdisk_label_get_parttype_from_code(lb, ustrtoull(part->type, NULL, 16));
		}
		ret = diskpart_set_partition(newpa, part, sector_size, parttype, oldtb->parent);
		if (ret) {
			WARN("I cannot set all partition's parameters");
		}
		if ((ret = fdisk_table_add_partition(tb->parent, newpa)) < 0) {
			ERROR("I cannot add partition %zu(%s): %d", part->partno, part->name, ret);
		}
		fdisk_unref_partition(newpa);
		if (ret)
			return ret;
	}

	/*
	 * Reload parent table against the context to populate default values.
	 * We must do this before adding hybrid entries so we can derive nested values.
	 */
	ret = diskpart_reload_table(PARENT(cxt), tb->parent);
	if (ret)
		return ret;

	if (IS_HYBRID(cxt)) {
		lb = fdisk_get_label(cxt, "dos");
		if (!lb) {
			ERROR("Failed to load hybrid label");
			return -EINVAL;
		}

		LIST_FOREACH(part, &priv.listparts, next) {
			if (strlen(part->dostype)) {
				struct fdisk_partition *newpa;

				newpa = fdisk_new_partition();

				parttype = fdisk_label_get_parttype_from_code(lb, ustrtoull(part->dostype, NULL, 16));
				if (!parttype) {
					ERROR("I cannot add hybrid partition %zu(%s) invalid dostype: %s",
						part->partno, part->name, part->dostype);
				}
				ret = diskpart_set_hybrid_partition(newpa, part, parttype, tb->parent);
				if (ret) {
					WARN("I cannot set all hybrid partition's parameters");
				}
				if ((ret = fdisk_table_add_partition(tb->child, newpa)) < 0) {
					ERROR("I cannot add hybrid partition %zu(%s): %d", part->partno, part->name, ret);
				}
				fdisk_unref_partition(newpa);
				if (ret)
					return ret;
			}
		}
		/*
		 * Add PMBR after other entries since bootloaders should not care about its position.
		 */
		ret = diskpart_append_hybrid_pmbr(lb, tb->child);
		if (ret)
			return ret;
		/*
		 * Reload child table against the context to fully populate remaining values.
		 */
		ret = diskpart_reload_table(cxt, tb->child);
		if (ret)
			return ret;
	} else {
		if (fdisk_is_label(cxt, DOS)) {
			LIST_FOREACH(part, &priv.listparts, next) {
				if (part->flags & DOS_FLAG_ACTIVE) {
					fdisk_toggle_partition_flag(cxt, part->partno, DOS_FLAG_ACTIVE);
				}
			}
		}
	}
	return ret;
}

/*
 * Return 1 if table differs, 0 if table is the same, negative on error
 */
static int diskpart_table_cmp(struct fdisk_context *cxt, struct fdisk_table *tb, struct fdisk_table *oldtb)
{
	size_t numnewparts = fdisk_table_get_nents(tb);
	size_t numpartondisk = fdisk_table_get_nents(oldtb);
	unsigned long i;
	int ret = 0;

	if (numpartondisk != numnewparts) {
		TRACE("Number of partitions differs on disk: %lu <--> requested: %lu",
			  (long unsigned int)numpartondisk, (long unsigned int)numnewparts);
		ret = 1;
	} else {
		struct fdisk_partition *pa, *newpa;
		struct fdisk_iter *itr	 = fdisk_new_iter(FDISK_ITER_FORWARD);
		struct fdisk_iter *olditr = fdisk_new_iter(FDISK_ITER_FORWARD);

		i = 0;
		while (i < numpartondisk && !ret) {
			newpa=NULL;
			pa = NULL;
			if (fdisk_table_next_partition (tb, itr, &newpa) ||
				fdisk_table_next_partition (oldtb, olditr, &pa)) {
				TRACE("Partition not found, something went wrong %lu !", i);
				ret = -EFAULT;
			} else if (is_diskpart_different(pa, newpa)) {
				TRACE("Partition differ:");
				diskpart_partition_info(cxt, "Original", pa);
				diskpart_partition_info(cxt, "New", newpa);
				ret = 1;
			}

			fdisk_unref_partition(newpa);
			fdisk_unref_partition(pa);
			i++;
		}
		fdisk_free_iter(itr);
		fdisk_free_iter(olditr);
	}
	return ret;
}

static int diskpart_compare_tables(struct fdisk_context *cxt, struct diskpart_table *tb,
		struct diskpart_table *oldtb, struct create_table *createtable)
{
	int ret = 0;

	/*
	 * A partiton table was found on disk, now compares the two tables
	 * to check if they differ.
	 */
	if (!createtable->parent) {
		ret = diskpart_table_cmp(PARENT(cxt), tb->parent, oldtb->parent);
		if (ret < 0)
			return ret;
		else if (ret)
			createtable->parent = true;
	}

	if (tb->child && !createtable->child) {
		ret = diskpart_table_cmp(cxt, tb->child, oldtb->child);
		if (ret < 0)
			return ret;
		else if (ret)
			createtable->child = true;
	}

	ret = 0;

	return ret;
}

static int diskpart_blkdev_lock(struct fdisk_context *cxt, bool nolock, bool noinuse)
{
	int oper = LOCK_EX;
	int ret = 0;

	if (fdisk_device_is_used(cxt)) {
		if (noinuse) {
			WARN("%s: device is in use, force set", fdisk_get_devname(cxt));

		} else {
			ERROR("%s: device is in use", fdisk_get_devname(cxt));
			return -EBUSY;
		}
	}

	if (!fdisk_is_readonly(cxt)) {
		ret = flock(fdisk_get_devfd(cxt), oper);
		if (ret) {
			switch (errno) {
				case EWOULDBLOCK:
					if (!nolock)
						ERROR("%s: device already locked", fdisk_get_devname(cxt));
					else
						WARN("%s: device already locked, nolock set", fdisk_get_devname(cxt));
					break;
				default:
					if (!nolock)
						ERROR("%s: failed to get lock", fdisk_get_devname(cxt));
					else
						WARN("%s: failed to get lock, nolock set", fdisk_get_devname(cxt));
			}
			if (!nolock)
				return -EBUSY;
		}
	}

	return 0;
}

static int diskpart_write_table(struct fdisk_context *cxt, struct create_table *createtable, bool nolock, bool noinuse)
{
	int ret = 0;

	if (createtable->parent || createtable->child) {
		TRACE("Partitions on disk differ, write to disk;");
		ret = diskpart_blkdev_lock(PARENT(cxt), nolock, noinuse);
		if (ret)
			return ret;
	} else {
		TRACE("Same partition table on disk, do not touch partition table !");
	}

	if (createtable->child) {
		if (!IS_HYBRID(cxt)) {
			ERROR("Internal fault, tried to create nested table but disk is not hybrid.");
			return -EINVAL;
		}
		/*
		 * Everything done, write into disk
		 */
		ret = fdisk_write_disklabel(cxt);
		if (ret)
			ERROR("Nested partition table cannot be written on disk");
		if (fdisk_reread_partition_table(cxt))
			WARN("Nested partition table cannot be reread from the disk, be careful !");
		if (ret)
			return ret;
	}

	if (createtable->parent) {
		/*
		 * Everything done, write into disk
		 */
		ret = fdisk_write_disklabel(PARENT(cxt));
		if (ret)
			ERROR("Partition table cannot be written on disk");
		if (fdisk_reread_partition_table(PARENT(cxt)))
			WARN("Table cannot be reread from the disk, be careful !");
		if (ret)
			return ret;
	}

	return ret;
}

static void diskpart_unref_context(struct fdisk_context *cxt)
{
	if (IS_HYBRID(cxt))
		fdisk_unref_context(PARENT(cxt));
	fdisk_unref_context(cxt);
}

static int diskpart_reread_partition(char *device)
{
	int fd, ret = -1;

	fd = open(device, O_RDONLY);
	if (fd < 0) {
		ERROR("Device %s can't be opened", device);
		goto out;
	}

	ret = ioctl(fd, BLKRRPART, NULL);
	if (ret < 0) {
		ERROR("Scan cannot be done on device %s", device);
		goto out_close;
	}

 out_close:
	close(fd);

 out:
	return ret;
}

static int keep_directory(const struct dirent *ent)
{
	return ((ent->d_type & DT_DIR) && \
		strcmp(ent->d_name, ".") && \
		strcmp(ent->d_name, ".."));
}

static char *compute_sys_block_path(char *device_name)
{
	int size = strlen("/sys/block/") + strlen(device_name) + 1;
	char *sys_block = malloc(size * sizeof(char));

	if (!sys_block) {
		ERROR("ERROR: cannot allocate sys_block\n");
		goto out;
	}

	sprintf(sys_block, "/sys/block/%s", device_name);

 out:
	return sys_block;
}

static int read_partition(char *sys_block, char *dir_name)
{
	size_t size = strlen(sys_block) + strlen(dir_name) + 12;
	char *path = (char *)malloc(size * sizeof(char));
	struct stat statbuf;
	int fd, err, len, partition = -1;
	char *data = NULL;

	if (!path) {
		ERROR("Failed to allocate path\n");
		goto out;
	}

	size = sprintf(path, "%s/%s/partition", sys_block, dir_name);

	fd = open(path, O_RDONLY);
	if (fd < 0)
		goto out;

	err = fstat(fd, &statbuf);
	if (err < 0) {
		ERROR("Cannot get info on %s\n", path);
		close(fd);
		goto out;
	}

	size = statbuf.st_size;
	data = (char *)malloc(size * sizeof(char));
	if (!data) {
		ERROR("Cannot allocate data\n");
		close(fd);
		goto out;
	}

	len = read(fd, data, size);
	if ((len > 0) && (len <= size))
		partition = strtoul(data, NULL, 10);

	close(fd);

 out:
	free(path);
	free(data);
	return partition;
}

static int set_partition(char *buf, int bufsize, char *device, int partno)
{
	char *device1 = strdup(device);
	char *device2 = strdup(device);
	char *device_path;
	char *device_name;
	char *sys_block = NULL;
	struct dirent **dirlist = NULL;
	int i, num_dir, ret = -1;

	if (!device1 || !device2) {
		ERROR("Cannot duplicate device\n");
		goto out;
	}

	device_path = dirname(device1);
	device_name = basename(device2);

	if (!device_name) {
		ERROR("Cannot get basename\n");
		goto out;
	}

	sys_block = compute_sys_block_path(device_name);
	if (!sys_block)
		goto out;

	num_dir = scandir(sys_block, &dirlist, keep_directory, NULL);
	if (num_dir < 0)
		goto out;

	for (i = 0; i < num_dir; i++) {
		char *dir_name = dirlist[i]->d_name;
		int partition;

		partition = read_partition(sys_block, dir_name);
		if (partition == partno) {
			snprintf(buf, bufsize, "%s/%s", device_path, dir_name);
			ret = 0;
		}

		free(dirlist[i]);
	}

	free(dirlist);

 out:
	free(sys_block);
	free(device1);
	free(device2);

	return ret;
}

static int install_gpt_partition_image(struct img_type *img,
				       void __attribute__ ((__unused__)) *data)
{
	struct fdisk_context *cxt = NULL;
	struct diskpart_table *tb = NULL;
	int ret = 0;
	unsigned long hybrid = 0;
	struct hnd_priv priv =  {
		.labeltype = FDISK_DISKLABEL_DOS,
	};
	struct create_table *createtable = NULL;
	struct fdisk_partition *pa;
	size_t partno;
	char device[MAX_VOLNAME];
	struct installer_handler *hnd;

	LIST_INIT(&priv.listparts);
	if (!strlen(img->device)) {
		ERROR("Partition handler without setting the device");
		return -EINVAL;
	}

	createtable = calloc(1, sizeof(*createtable));
	if (!createtable) {
		ERROR("OOM allocating createtable !");
		return -ENOMEM;
	}

	/*
	 * Reads flags: nolock and noinuse
	 */
	priv.nolock = strtobool(dict_get_value(&img->properties, "nolock"));
	priv.noinuse = strtobool(dict_get_value(&img->properties, "noinuse"));

        /*
         * Create context
         */
	ret = diskpart_assign_context(&cxt, img, priv, hybrid, createtable);
	if (ret == -EACCES)
		goto handler_release;
	else if (ret)
		goto handler_exit;

	tb = calloc(1, sizeof(*tb));
	if (!tb) {
		ERROR("OOM loading partitions !");
		ret = -ENOMEM;
		goto handler_exit;
	}

	/*
	 * Fill the in-memory partition table from the disk.
	 */
	ret = diskpart_get_partitions(cxt, tb, createtable);
	if (ret)
		goto handler_exit;

	/*
	 * Search partition to update
	 */
	pa = diskpart_get_partition_by_name(tb, img->volname);
	if (!pa) {
		ERROR("Can't find partition %s", img->volname);
		ret = -1;
		goto handler_exit;
	}

	/*
	 * Set device for next handler
	 */
	partno = fdisk_partition_get_partno(pa);
	set_partition(device, sizeof(device), img->device, partno + 1);
	strlcpy(img->device, device, sizeof(img->device));

	/*
	 * Set next handler
	 */
	strcpy(img->type, "raw");

	/*
	 * Search next handler
	 */
	hnd = find_handler(img);
	if (!hnd) {
		ERROR("Can't find handler raw\n");
		goto handler_exit;
	}

	/*
	 * Launch next handler
	 */
	ret = hnd->installer(img, NULL);

handler_exit:
	if (tb)
		diskpart_unref_table(tb);
	if (cxt && fdisk_get_devfd(cxt) >= 0)
		if (fdisk_deassign_device(cxt, 0))
			WARN("Error deassign device %s", img->device);

handler_release:
	if (cxt)
		diskpart_unref_context(cxt);

	if (createtable)
		free(createtable);

	/*
	 * Declare that handler has finished
	 */
	swupdate_progress_update(100);

	return ret;
}

static int diskpart(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	struct dict_list *parts;
	struct dict_list_elem *elem;
	struct fdisk_context *cxt = NULL;
	struct partition_data *part;
	struct partition_data *tmp;
	struct diskpart_table *tb = NULL;
	struct diskpart_table *oldtb = NULL;
	int ret = 0;
	unsigned long i;
	unsigned long hybrid = 0;
	unsigned int nbootflags = 0;
	struct hnd_priv priv =  {
		.labeltype = FDISK_DISKLABEL_DOS,
	};
	struct create_table *createtable = NULL;

	if (!diskpart_is_gpt(img) && !diskpart_is_dos(img)) {
		ERROR("Just GPT or DOS partition table are supported");
		return -EINVAL;
	}
	LIST_INIT(&priv.listparts);
	if (!strlen(img->device)) {
		ERROR("Partition handler without setting the device");
		return -EINVAL;
	}

	createtable = calloc(1, sizeof(*createtable));
	if (!createtable) {
		ERROR("OOM allocating createtable !");
		return -ENOMEM;
	}

	/*
	 * Reads flags: nolock and noinuse
	 */
	priv.nolock = strtobool(dict_get_value(&img->properties, "nolock"));
	priv.noinuse = strtobool(dict_get_value(&img->properties, "noinuse"));

	/*
	 * Parse partitions
	 */
	struct dict_entry *entry;
	LIST_FOREACH(entry, &img->properties, next) {
		parts = &entry->list;
		if (!parts)
			continue;

		if (strncmp(dict_entry_get_key(entry),
				"partition-", strlen("partition-")))
			continue;

		part = (struct partition_data *)calloc(1, sizeof(struct partition_data));
		if (!part) {
			ERROR("FAULT: no memory");
			ret = -ENOMEM;
			goto handler_exit;
		}
		elem = LIST_FIRST(parts);

		part->partno = LIBFDISK_INIT_UNDEF(part->partno);
		part->start = LIBFDISK_INIT_UNDEF(part->start);
		part->size = LIBFDISK_INIT_UNDEF(part->size);
		part->force = 0;

		part->partno = strtoul(entry->key  + strlen("partition-"), NULL, 10);
		while (elem) {
			char *equal = index(elem->value, '=');
			if (equal) {
				for (i = 0; i < ARRAY_SIZE(fields); i++) {
					if (!((equal - elem->value) == strlen(fields[i]) &&
						!strncmp(elem->value, fields[i], strlen(fields[i]))))
						continue;
					equal++;
					switch (i) {
					case PART_SIZE:
						part->size = ustrtoull(equal, NULL, 10);
						if (!size_delimiter_match(equal))
							part->explicit_size = 1;
						break;
					case PART_START:
						part->start = ustrtoull(equal, NULL, 10);
						break;
					case PART_TYPE:
						strncpy(part->type, equal, sizeof(part->type));
						break;
					case PART_NAME:
						strncpy(part->name, equal, sizeof(part->name));
						break;
					case PART_FSTYPE:
#ifdef CONFIG_DISKPART_FORMAT
						strncpy(part->fstype, equal, sizeof(part->fstype));
						break;
#else
						ERROR("Partitions have fstype entries but diskpart format support is missing !");
						ret = -EINVAL;
						goto handler_exit;
#endif
					case PART_DOSTYPE:
						strncpy(part->dostype, equal, sizeof(part->dostype));
						hybrid++;
						break;
					case PART_UUID:
						strncpy(part->partuuid, equal, sizeof(part->partuuid));
						break;
					case PART_FLAG:
						if (strcmp(equal, "boot")) {
							ERROR("Unknown flag : %s", equal);
							ret = -EINVAL;
							goto handler_exit;
						}
						nbootflags++;
						if (nbootflags > 1) {
							ERROR("Boot flag set to multiple partitions");
							ret = -EINVAL;
							goto handler_exit;
						}
						part->flags |= DOS_FLAG_ACTIVE;
						break;
					case PART_FORCE:
						part->force = strtobool(equal);
						TRACE("Force flag explicitly mentioned, value %d", part->force);
						break;
					}
				}
			}
			elem = LIST_NEXT(elem, next);
		}

		/*
		 * Hybrid entries must use the primary DOS/MBR partition table,
		 * this has a maximum of four partitions, however we must reserve
		 * one for the hybrid PMBR entry so that GPT aware software will
		 * read the GPT table properly.
		 */
		if (hybrid > 3) {
			ERROR("I cannot add hybrid partition %zu(%s): hybrid dos partition limit of 3 exceeded",
				  part->partno, strlen(part->name) ? part->name : "UNDEF NAME");
			ret = -EINVAL;
			goto handler_exit;
		}

		TRACE("partition-%zu:%s size %" PRIu64 " start %zu type %s partuuid %s",
			part->partno != LIBFDISK_INIT_UNDEF(part->partno) ? part->partno : 0,
			strlen(part->name) ? part->name : "UNDEF NAME",
			part->size != LIBFDISK_INIT_UNDEF(part->size) ? part->size : 0,
			part->start!= LIBFDISK_INIT_UNDEF(part->start) ? part->start : 0,
			part->type,
			strlen(part->partuuid) ? part->partuuid : "automatic");

		/*
		 * Partitions in sw-description start from 1,
		 * libfdisk first partition is 0
		 */
		if (part->partno > 0)
			part->partno--;

		/*
		 * Insert the partition in the list sorted by partno
		 */
		struct partition_data *p = LIST_FIRST(&priv.listparts);
		if (!p)
			LIST_INSERT_HEAD(&priv.listparts, part, next);
		else {
			while (LIST_NEXT(p, next) &&
					LIST_NEXT(p, next)->partno < part->partno)
				p = LIST_NEXT(p, next);
			LIST_INSERT_BEFORE(p, part, next);
		}
	}

	if (hybrid && !diskpart_is_gpt(img)) {
		ERROR("Partitions have hybrid(dostype) entries but labeltype is not gpt !");
		ret = -EINVAL;
		goto handler_release;
	}
	if (nbootflags && !diskpart_is_dos(img)) {
		ERROR("Boot flag can be set just for labeltype dos !");
		ret = -EINVAL;
		goto handler_release;
	}

	ret = diskpart_assign_context(&cxt, img, priv, hybrid, createtable);
	if (ret == -EACCES)
		goto handler_release;
	else if (ret)
		goto handler_exit;

	/*
	 * Create a new in-memory partition table to be compared
	 * with the table on the disk, and applied if differs
	 */
	tb = diskpart_new_table(cxt);
	if (!tb) {
		ERROR("OOM creating new table !");
		ret = -ENOMEM;
		goto handler_exit;
	}

	oldtb = calloc(1, sizeof(*oldtb));
	if (!oldtb) {
		ERROR("OOM loading partitions !");
		ret = -ENOMEM;
		goto handler_exit;
	}

	/*
	 * Fill the old in-memory partition table from the disk.
	 */
	ret = diskpart_get_partitions(cxt, oldtb, createtable);
	if (ret)
		goto handler_exit;

	/*
	 * Fill the new in-memory partition table from the partition list.
	 */
	ret = diskpart_fill_table(cxt, tb, oldtb, part, priv);
	if (ret)
		goto handler_exit;

	ret = diskpart_compare_tables(cxt, tb, oldtb, createtable);
	if (ret)
		goto handler_exit;

	ret = diskpart_write_table(cxt, createtable, priv.nolock, priv.noinuse);

handler_exit:
	if (tb)
		diskpart_unref_table(tb);
	if (oldtb)
		diskpart_unref_table(oldtb);
	if (cxt && fdisk_get_devfd(cxt) >= 0)
		if (fdisk_deassign_device(cxt, 0))
			WARN("Error deassign device %s", img->device);

handler_release:
	if (cxt)
		diskpart_unref_context(cxt);

	/*
	 * Kernel rereads the partition table and add just a delay to be sure
	 * that SWUpdate does not try to access the partitions before the kernel is
	 * ready
	 */

	sleep(2);

#ifdef CONFIG_DISKPART_FORMAT
	/* Create filesystems */
	if (!ret) {
		LIST_FOREACH(part, &priv.listparts, next) {
			/*
			 * priv.listparts counts partitions starting with 0,
			 * but fdisk_partname expects the first partition having
			 * the number 1.
			 */
			size_t partno = part->partno + 1;

			if (!strlen(part->fstype))
				continue;  /* Don't touch partitions without fstype */

			char *path = NULL;
			char *device = NULL;

			path = realpath(img->device, NULL);
			if (!path)
				path = strdup(img->device);
			device = fdisk_partname(path, partno);
			free(path);

			if (!createtable->parent && !part->force) {
				/* Check if file system exists */
				ret = diskformat_fs_exists(device, part->fstype);

				if (ret < 0) {
					free(device);
					break;
				}

				if (ret) {
					TRACE("Found %s file system on %s, skip mkfs",
						  part->fstype, device);
					ret = 0;
					free(device);
					continue;
				}
			}

			ret = diskformat_mkfs(device, part->fstype);
			free(device);
			if (ret)
				break;
		}
	}
#endif

	LIST_FOREACH_SAFE(part, &priv.listparts, next, tmp) {
		LIST_REMOVE(part, next);
		free(part);
	}

	if (createtable)
		free(createtable);

	/*
	 * Declare that handler has finished
	 */
	swupdate_progress_update(100);

	return ret;
}

static int toggle_boot(struct img_type *img, void  *data)
{

	struct fdisk_context *cxt;
	char *path;
	int ret;
	unsigned long partno;

	struct script_handler_data *script_data;

	if (!data)
		return -1;

	script_data = data;

	if (script_data->scriptfn != POSTINSTALL)
		return 0;

	/*
	 * Parse properties
	 */
	partno = strtoul(dict_get_value(&img->properties, "partition"), NULL, 10);

	/*
	 * Set is possible only for primary partitions
	 */
	if (partno > 4 || partno < 1) {
		ERROR("Wrong partition number: %ld", partno);
		return -EINVAL;
	}

	partno--;

	cxt = fdisk_new_context();
	if (!cxt) {
		ERROR("Failed to allocate libfdisk context");
		return -ENOMEM;
	}

	/*
	 * Resolve device path symlink.
	 */
	path = realpath(img->device, NULL);
	if (!path)
		path = strdup(img->device);

	/*
	 * fdisk_new_nested_context requires the device to be assigned.
	 */
	ret = fdisk_assign_device(cxt, path, 0);
	free(path);
	if (ret < 0) {
		ERROR("Device %s cannot be opened: %s", img->device, strerror(-ret));
		return ret;
	}

	if (!fdisk_is_label(cxt, DOS)) {
		ERROR("Setting boot flag supported for DOS table only");
		ret = -EINVAL;
		goto toggle_boot_exit;
	}

	int nparts = fdisk_get_npartitions(cxt);

	TRACE("Toggling Boot Flag for partition %ld", partno);

	struct fdisk_partition *pa = NULL;
	for (int i = 0; i < nparts; i++) {

		if (fdisk_get_partition(cxt, i, &pa) != 0)
			continue;

		if (i != partno) {
			if (fdisk_partition_is_bootable(pa))
				fdisk_toggle_partition_flag(cxt, i, DOS_FLAG_ACTIVE);
		} else {
			if (!fdisk_partition_is_bootable(pa)) {
				ret = fdisk_toggle_partition_flag(cxt, i, DOS_FLAG_ACTIVE);
				if (ret)
					ERROR("Setting boot flag for partition %d on %s FAILED", i, img->device);
			}
		}
	}
	fdisk_unref_partition(pa);

	ret = fdisk_write_disklabel(cxt);

toggle_boot_exit:
	if (cxt && fdisk_get_devfd(cxt) >= 0) {
		if (fdisk_deassign_device(cxt, 0))
			WARN("Error deassign device %s", img->device);
	}
	if (cxt)
		diskpart_unref_context(cxt);

	/*
	 * Declare that handler has finished
	 */
	swupdate_progress_update(100);

	return ret;
}

static int gpt_swap_partition(struct img_type *img, void *data)
{
	struct fdisk_context *cxt = NULL;
	struct diskpart_table *tb = NULL;
	int ret = 0;
	unsigned long hybrid = 0;
	struct hnd_priv priv =  {
		.labeltype = FDISK_DISKLABEL_DOS,
	};
	struct create_table *createtable = NULL;
	struct script_handler_data *script_data = data;
	char prop[SWUPDATE_GENERAL_STRING_SIZE];
	struct dict_list *partitions;
	struct dict_list_elem *partition;
	int num, count = 0;
	char *name[2];

	if (!script_data)
		return -EINVAL;

	/*
	 * Call only in case of postinstall
	 */
	if (script_data->scriptfn != POSTINSTALL)
		return 0;

	LIST_INIT(&priv.listparts);
	if (!strlen(img->device)) {
		ERROR("Partition handler without setting the device");
		return -EINVAL;
	}

	createtable = calloc(1, sizeof(*createtable));
	if (!createtable) {
		ERROR("OOM allocating createtable !");
		return -ENOMEM;
	}

	/*
	 * Reads flags: nolock and noinuse
	 */
	priv.nolock = strtobool(dict_get_value(&img->properties, "nolock"));
	priv.noinuse = strtobool(dict_get_value(&img->properties, "noinuse"));

        /*
         * Create context
         */
	ret = diskpart_assign_context(&cxt, img, priv, hybrid, createtable);
	if (ret == -EACCES)
		goto handler_release;
	else if (ret)
		goto handler_exit;

	tb = calloc(1, sizeof(*tb));
	if (!tb) {
		ERROR("OOM loading partitions !");
		ret = -ENOMEM;
		goto handler_exit;
	}

	/*
	 * Fill the in-memory partition table from the disk.
	 */
	ret = diskpart_get_partitions(cxt, tb, createtable);
	if (ret)
		goto handler_exit;

	while (1) {
		snprintf(prop, sizeof(prop), "swap-%d", count);
		partitions = dict_get_list(&img->properties, prop);
		if (!partitions)
			break;

		num = 0;
		LIST_FOREACH(partition, partitions, next) {
			if (num >= 2) {
				ERROR("Too many partition (%s)", prop);
				goto handler_exit;
			}

			name[num] = partition->value;
			num++;
		}

		if (num != 2) {
			ERROR("Invalid number (%d) of partition (%s)", num, prop);
			goto handler_exit;
		}

		TRACE("swap partition %s <-> %s", name[0], name[1]);

		ret = diskpart_swap_partition(tb, createtable, name[0], name[1]);
		if (ret) {
			ERROR("Can't swap %s and %s", name[0], name[1]);
			break;
		}

		count++;
	}

	/* Reload table for parent */
	ret = diskpart_reload_table(PARENT(cxt), tb->parent);
	if (ret) {
		ERROR("Can't reload table for parent (err = %d)", ret);
		goto handler_exit;
	}

	/* Reload table for child */
	if (IS_HYBRID(cxt)) {
		ret = diskpart_reload_table(cxt, tb->child);
		if (ret) {
			ERROR("Can't reload table for child (err = %d)", ret);
			goto handler_exit;
		}
	}

	/* Write table */
	ret = diskpart_write_table(cxt, createtable, priv.nolock, priv.noinuse);
	if (ret) {
		ERROR("Can't write table (err = %d)", ret);
		goto handler_exit;
	}

	/*
	 * Declare that handler has finished
	 */
	swupdate_progress_update(100);

handler_exit:
	if (tb)
		diskpart_unref_table(tb);
	if (cxt && fdisk_get_devfd(cxt) >= 0)
		if (fdisk_deassign_device(cxt, 0))
			WARN("Error deassign device %s", img->device);

handler_release:
	if (cxt)
		diskpart_unref_context(cxt);

	if (createtable)
		free(createtable);

	/*
	 * Re-read the partition table to be sure that SWupdate does not
	 * try to acces the partitions before the kernel is ready
	 */
	diskpart_reread_partition(img->device);

	/*
	 * Declare that handler has finished
	 */
	swupdate_progress_update(100);

	return ret;
}

__attribute__((constructor))
void diskpart_handler(void)
{
	register_handler("diskpart", diskpart,
				PARTITION_HANDLER | NO_DATA_HANDLER, NULL);
}

__attribute__((constructor))
void diskpart_toggle_boot(void)
{
	register_handler("toggleboot", toggle_boot,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}

__attribute__((constructor))
void diskpart_gpt_swap_partition(void)
{
	register_handler("gptswap", gpt_swap_partition,
			 SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}

__attribute__((constructor))
void diskpart_install_gpt_partition_image(void)
{
	register_handler("gptpart", install_gpt_partition_image,
			 IMAGE_HANDLER, NULL);
}
