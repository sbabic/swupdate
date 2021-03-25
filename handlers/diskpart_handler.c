/*
 * (C) Copyright 2019
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <libfdisk/libfdisk.h>
#include "swupdate.h"
#include "handler.h"
#include "util.h"
#include "fatfs_interface.h"

void diskpart_handler(void);

/*
 * This is taken from libfdisk to declare if a field is not set
 */
#define LIBFDISK_INIT_UNDEF(_x)	((__typeof__(_x)) -1)

/* Linux native partition type */
 #define GPT_DEFAULT_ENTRY_TYPE "0FC63DAF-8483-4772-8E79-3D69D8477DE4"


/**
 * Keys for the properties field in sw-description
 */
enum partfield {
	PART_SIZE = 0,
	PART_START,
	PART_TYPE,
	PART_NAME,
	PART_FSTYPE
};

const char *fields[] = {
	[PART_SIZE] = "size",
	[PART_START] = "start",
	[PART_TYPE] = "type",
	[PART_NAME] = "name",
	[PART_FSTYPE] = "fstype"
};

struct partition_data {
	size_t partno;
	uint64_t size;
	size_t start;
	char type[SWUPDATE_GENERAL_STRING_SIZE];
	char name[SWUPDATE_GENERAL_STRING_SIZE];
	char fstype[SWUPDATE_GENERAL_STRING_SIZE];
	LIST_ENTRY(partition_data) next;
};
LIST_HEAD(listparts, partition_data);

/*
 * Internal handler data
 */
struct hnd_priv {
	enum fdisk_labeltype labeltype;
	struct listparts listparts;	/* list of partitions */
};

/**
 * diskpart_set_partition - set values in a fdisk_partition
 * @cxt: libfdisk context
 * @pa: pointer to fdisk_partition to be changed
 * @part: structure with values to be set, read from sw-description
 *
 * return 0 if ok
 */
static int diskpart_set_partition(struct fdisk_partition *pa,
				  struct partition_data *part,
				  unsigned long sector_size,
				  struct fdisk_parttype *parttype)
{
	int ret = 0;

	if (!sector_size)
		sector_size = 1;
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
		ret |= fdisk_partition_partno_follow_default(pa, 1);
	if (strlen(part->name))
	      ret |= fdisk_partition_set_name(pa, part->name);
	if (part->size != LIBFDISK_INIT_UNDEF(part->size))
	      ret |= fdisk_partition_set_size(pa, part->size / sector_size);
	else
		ret |= fdisk_partition_end_follow_default(pa, 1);

	if (parttype)
		ret |= fdisk_partition_set_type(pa, parttype);

	return ret;
}

/*
 * Return true if partition differs
 */
static bool diskpart_partition_cmp(const char *lbtype, struct fdisk_partition *firstpa, struct fdisk_partition *secondpa)
{
	if (!firstpa || !secondpa)
		return true;

	if (firstpa && secondpa && (fdisk_partition_cmp_partno(firstpa, secondpa) ||
		(!fdisk_partition_start_is_default(firstpa) && !fdisk_partition_start_is_default(secondpa) && 
		fdisk_partition_cmp_start(firstpa, secondpa)) ||
		(!strcmp(lbtype, "gpt") &&
			(strcmp(fdisk_parttype_get_string(fdisk_partition_get_type(firstpa)),
				fdisk_parttype_get_string(fdisk_partition_get_type(secondpa))) ||
			strcmp(fdisk_partition_get_name(firstpa) ? fdisk_partition_get_name(firstpa) : "",
		       		fdisk_partition_get_name(secondpa) ? fdisk_partition_get_name(secondpa) : ""))) ||
		(!strcmp(lbtype, "dos") &&
			fdisk_parttype_get_code(fdisk_partition_get_type(firstpa)) !=
			fdisk_parttype_get_code(fdisk_partition_get_type(secondpa))) ||
		fdisk_partition_get_size(firstpa) != fdisk_partition_get_size(secondpa))) {
		TRACE("Partition differ : %s(%llu) <--> %s(%llu)",
			fdisk_partition_get_name (firstpa) ? fdisk_partition_get_name(firstpa) : "",
			(long long unsigned)fdisk_partition_get_size(firstpa),
			fdisk_partition_get_name(secondpa) ? fdisk_partition_get_name(secondpa) : "",
			(long long unsigned)fdisk_partition_get_size(secondpa));
		return true;
	}
	return false;
}

static int diskpart(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	char *lbtype = dict_get_value(&img->properties, "labeltype");
	struct dict_list *parts;
	struct dict_list_elem *elem;
	struct fdisk_context *cxt;
	struct partition_data *part;
	struct partition_data *tmp;
	struct fdisk_table *tb = NULL;
	struct fdisk_table *oldtb = NULL;
	struct fdisk_parttype *parttype = NULL;
	int ret = 0;
	unsigned long i;
	struct hnd_priv priv =  {FDISK_DISKLABEL_DOS};
	bool createtable = false;

	if (!lbtype || (strcmp(lbtype, "gpt") && strcmp(lbtype, "dos"))) {
		ERROR("Just GPT or DOS partition table are supported");
		return -EINVAL;
	}
	LIST_INIT(&priv.listparts);
	if (!strlen(img->device)) {
		ERROR("Partition handler without setting the device");
		return -EINVAL;
	}

	cxt = fdisk_new_context();
	if (!cxt) {
		ERROR("Failed to allocate libfdisk context");
		return -ENOMEM;
	}

	ret = fdisk_assign_device(cxt, img->device, 0);
	if (ret == -EACCES) {
		ERROR("no access to %s", img->device);
		goto handler_release;
	}

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
						strncpy(part->fstype, equal, sizeof(part->fstype));
						break;
					}
				}
			}
			elem = LIST_NEXT(elem, next);
		}

		TRACE("partition-%zu:%s size %" PRIu64 " start %zu type %s",
			part->partno != LIBFDISK_INIT_UNDEF(part->partno) ? part->partno : 0,
			strlen(part->name) ? part->name : "UNDEF NAME",
			part->size != LIBFDISK_INIT_UNDEF(part->size) ? part->size : 0,
			part->start!= LIBFDISK_INIT_UNDEF(part->start) ? part->start : 0,
			part->type);

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

	/*
	 * Check partition table
	 */
	if (!fdisk_has_label(cxt)) {
		WARN("%s does not contain a recognized partition table",
		     img->device);
		fdisk_create_disklabel(cxt, lbtype);
		createtable = true;
	} else if (lbtype) {
		if (!strcmp(lbtype, "gpt"))
			priv.labeltype = FDISK_DISKLABEL_GPT;
		else
			priv.labeltype = FDISK_DISKLABEL_DOS;

		if (!fdisk_is_labeltype(cxt, priv.labeltype)) {
			WARN("Partition table of different type, setting to %s, all data lost !",
				lbtype);
			fdisk_create_disklabel(cxt, lbtype);
			createtable = true;
		}
	}

	struct fdisk_label *lb = fdisk_get_label(cxt, NULL);
	unsigned long sector_size = fdisk_get_sector_size(cxt);

	/*
	 * Create a new in-memory partition taböe to be compared
	 * with the table on the disk, and applied if differs
	 */
	tb = fdisk_new_table();

	if (fdisk_get_partitions(cxt, &oldtb))
		createtable = true;

	if (!tb) {
		ERROR("OOM creating new table !");
		ret = -ENOMEM;
		goto handler_exit;
	}

	i = 0;

	LIST_FOREACH(part, &priv.listparts, next) {
		struct fdisk_partition *newpa;

		newpa = fdisk_new_partition();
		/*
	 	 * GPT uses strings instead of hex code for partition type
	 	*/
		if (fdisk_is_label(cxt, GPT)) {
			parttype = fdisk_label_get_parttype_from_string(lb, part->type); 
			if (!parttype)
				parttype = fdisk_label_get_parttype_from_string(lb, GPT_DEFAULT_ENTRY_TYPE); 
		} else {
			parttype = fdisk_label_get_parttype_from_code(lb, ustrtoull(part->type, 16));
		}
		ret = diskpart_set_partition(newpa, part, sector_size, parttype);
		if (ret) {
			WARN("I cannot set all partition's parameters");
		}
		if ((ret = fdisk_table_add_partition(tb, newpa)) < 0) {
			ERROR("I cannot add partition %zu(%s): %d", part->partno, part->name, ret);
		}
		fdisk_unref_partition(newpa);
		if (ret < 0)
			goto handler_exit;
		i++;
	}

	/*
	 * A partiton table was found on disk, now compares the two tables
	 * to check if they differ.
	 */
	if (!createtable) {
		size_t numpartondisk = fdisk_table_get_nents(oldtb);

		if (numpartondisk != i) {
			TRACE("Number of partitions differs on disk: %lu <--> requested: %lu",
				(long unsigned int)numpartondisk, i);
			createtable = true;
		} else {
			struct fdisk_partition *pa, *newpa;
			struct fdisk_iter *itr	 = fdisk_new_iter(FDISK_ITER_FORWARD);
			struct fdisk_iter *olditr = fdisk_new_iter(FDISK_ITER_FORWARD);

			i = 0;
			while (i < numpartondisk && !createtable) {
				newpa=NULL;
				pa = NULL;
				if (fdisk_table_next_partition (tb, itr, &newpa) ||
					fdisk_table_next_partition (oldtb, olditr, &pa)) {
					TRACE("Partition not found, something went wrong %lu !", i);
					ret = -EFAULT;
					goto handler_exit;
				}
				if (diskpart_partition_cmp(lbtype, pa, newpa)) {
					createtable = true;
				}

				fdisk_unref_partition(newpa);
				fdisk_unref_partition(pa);
				i++;
			}
		}
	}

	if (createtable) {
		TRACE("Partitions on disk differ, write to disk;");
		fdisk_delete_all_partitions(cxt);
		ret = fdisk_apply_table(cxt, tb);
		if (ret) {
			ERROR("Partition table cannot be applied !");
			goto handler_exit;
		}

		/*
		 * Everything done, write into disk
		 */
		ret = fdisk_write_disklabel(cxt);
		if (ret)
			ERROR("Partition table cannot be written on disk");
		if (fdisk_reread_partition_table(cxt))
			WARN("Table cannot be reread from the disk, be careful !");
	} else {
		ret = 0;
		TRACE("Same partition table on disk, do not touch partition table !");
	}

#ifdef CONFIG_DISKFORMAT
	/* Create filesystems */
	LIST_FOREACH(part, &priv.listparts, next) {
		/*
		 * priv.listparts counts partitions starting with 0,
		 * but fdisk_partname expects the first partition having
		 * the number 1.
		 */
		size_t partno = part->partno + 1;

		if (!strlen(part->fstype))
			continue;  /* Don't touch partitions without fstype */

#ifdef CONFIG_FAT_FILESYSTEM
		if (!strcmp(part->fstype, "vfat")) {
			char *device = NULL;
			device = fdisk_partname(img->device, partno);
			TRACE("Creating vfat file system on partition-%lu, device %s", partno, device);
			ret = fat_mkfs(device);
			if (ret)
				ERROR("creating vfat file system failed. %d", ret);
			free(device);
			continue;
		}
#endif
		ERROR("partition-%lu %s filesystem type not supported.", partno, part->fstype);
	}
#endif

handler_exit:
	if (tb)
		fdisk_unref_table(tb);
	if (oldtb)
		fdisk_unref_table(oldtb);
	if (fdisk_deassign_device(cxt, 0))
		WARN("Error deassign device %s", img->device);

handler_release:
	LIST_FOREACH_SAFE(part, &priv.listparts, next, tmp) {
		LIST_REMOVE(part, next);
		free(part);
	}

	fdisk_unref_context(cxt);

	return ret;
}

__attribute__((constructor))
void diskpart_handler(void)
{
	register_handler("diskpart", diskpart,
				PARTITION_HANDLER | NO_DATA_HANDLER, NULL);
}
