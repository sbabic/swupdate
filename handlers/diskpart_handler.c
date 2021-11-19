/*
 * (C) Copyright 2019
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
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
#include <sys/types.h>
#include <libfdisk/libfdisk.h>
#include <fs_interface.h>
#include <uuid/uuid.h>
#include "swupdate.h"
#include "handler.h"
#include "util.h"

void diskpart_handler(void);

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
};

const char *fields[] = {
	[PART_SIZE] = "size",
	[PART_START] = "start",
	[PART_TYPE] = "type",
	[PART_NAME] = "name",
	[PART_FSTYPE] = "fstype",
	[PART_DOSTYPE] = "dostype",
	[PART_UUID] = "partuuid",
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
		if (!strcmp(lbtype, "gpt")) {
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
	if (ret == -EACCES) {
		ERROR("no access to %s", img->device);
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
	if (part->size != LIBFDISK_INIT_UNDEF(part->size))
	      ret |= fdisk_partition_set_size(pa, part->size / sector_size);
	else
		ret |= fdisk_partition_end_follow_default(pa, 1);

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
			parttype = fdisk_label_get_parttype_from_string(lb, part->type);
			if (!parttype)
				parttype = fdisk_label_get_parttype_from_string(lb, GPT_DEFAULT_ENTRY_TYPE);
		} else {
			parttype = fdisk_label_get_parttype_from_code(lb, ustrtoull(part->type, 16));
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

				parttype = fdisk_label_get_parttype_from_code(lb, ustrtoull(part->dostype, 16));
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

static int diskpart(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	char *lbtype = diskpart_get_lbtype(img);
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
	struct hnd_priv priv =  {
		.labeltype = FDISK_DISKLABEL_DOS,
	};
	struct create_table *createtable = NULL;

	if (!lbtype || (strcmp(lbtype, "gpt") && strcmp(lbtype, "dos"))) {
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
						part->size = ustrtoull(equal, 10);
						break;
					case PART_START:
						part->start = ustrtoull(equal, 10);
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

	if (hybrid && (!lbtype || strcmp(lbtype, "gpt"))) {
		ERROR("Partitions have hybrid(dostype) entries but labeltype is not gpt !");
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

			if (!createtable->parent) {
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

	return ret;
}

__attribute__((constructor))
void diskpart_handler(void)
{
	register_handler("diskpart", diskpart,
				PARTITION_HANDLER | NO_DATA_HANDLER, NULL);
}
