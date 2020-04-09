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

void diskpart_handler(void);

/**
 * Keys for the properties field in sw-description
 */
enum partfield {
	PART_SIZE = 0,
	PART_START,
	PART_TYPE,
	PART_NAME
};

const char *fields[] = {
	[PART_SIZE] = "size",
	[PART_START] = "start",
	[PART_TYPE] = "type",
	[PART_NAME] = "name"
};

struct partition_data {
	size_t partno;
	uint64_t size;
	size_t start;
	char type[SWUPDATE_GENERAL_STRING_SIZE];
	char name[SWUPDATE_GENERAL_STRING_SIZE];
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
static int diskpart_set_partition(struct fdisk_context *cxt,
				  struct fdisk_partition *pa,
				  struct partition_data *part)
{
	unsigned long sector_size = fdisk_get_sector_size(cxt);
	struct fdisk_label *lb;
	struct fdisk_parttype *parttype = NULL;
	int ret;

	lb = fdisk_get_label(cxt, NULL);

	if (!sector_size)
		sector_size = 1;
	ret = fdisk_partition_set_partno(pa, part->partno) ||
	      fdisk_partition_set_size(pa, part->size / sector_size) ||
	      fdisk_partition_set_name(pa, part->name) ||
	      fdisk_partition_set_start(pa, part->start);

	/*
	 * GPT uses strings instead of hex code for partition type
	 */
	if (fdisk_is_label(cxt, GPT)) {
		parttype = fdisk_label_get_parttype_from_string(lb, part->type); 
	} else if (fdisk_is_label(cxt, DOS)) {
		parttype = fdisk_label_get_parttype_from_code(lb, ustrtoull(part->type, 16));
	} else
		WARN("Partition type set just for GPT or DOS");

	if (parttype)
		ret |= fdisk_partition_set_type(pa, parttype);
	return ret;
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
	int ret = 0;
	int i;
	struct hnd_priv priv =  {FDISK_DISKLABEL_DOS};

	if (lbtype && strcmp(lbtype, "gpt") && strcmp(lbtype, "dos")) {
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
					}
				}
			}
			elem = LIST_NEXT(elem, next);
		}

		TRACE("partition-%zu:%s size %" PRIu64 " start %zu type %s",
			part->partno,
			part->name,
			part->size,
			part->start,
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
	} else if (lbtype) {
		if (!strcmp(lbtype, "gpt"))
			priv.labeltype = FDISK_DISKLABEL_GPT;
		else
			priv.labeltype = FDISK_DISKLABEL_DOS;

		if (!fdisk_is_labeltype(cxt, priv.labeltype)) {
			WARN("Partition table of different type, setting to %s, all data lost !",
				lbtype);
			fdisk_create_disklabel(cxt, lbtype);
		}
	}

	i = 0;

	if (priv.labeltype == FDISK_DISKLABEL_DOS) {
		fdisk_delete_all_partitions(cxt);
	}

	LIST_FOREACH(part, &priv.listparts, next) {
		struct fdisk_partition *pa = NULL;
		size_t partno;

		/*
		 * Allow to have not consecutives partitions
		 */
		if (part->partno > i) {
			while (i < part->partno) {
				TRACE("DELETE PARTITION %d", i);
				fdisk_delete_partition(cxt, i);
				i++;
			}
		}

		if (fdisk_get_partition(cxt, part->partno, &pa)) {
			struct fdisk_partition *newpa;
			newpa = fdisk_new_partition();
			ret = diskpart_set_partition(cxt, newpa, part);
			if (ret) {
				WARN("I cannot set all partition's parameters");
			}
			if (fdisk_add_partition(cxt, newpa, &partno) < 0) {
				ERROR("I cannot add partition %zu(%s)", part->partno, part->name);
			}
			fdisk_unref_partition(newpa);
		} else {
			ret = diskpart_set_partition(cxt, pa, part);
			if (ret) {
				WARN("I cannot set all partition's parameters");
			}
			if (fdisk_set_partition(cxt, part->partno, pa) < 0) {
				ERROR("I cannot modify partition %zu(%s)", part->partno, part->name);
			}
			fdisk_unref_partition(pa);
		}
		i++;
	}

	/*
	 * Everything done, write into disk
	 */
	ret = fdisk_write_disklabel(cxt);

handler_exit:
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
