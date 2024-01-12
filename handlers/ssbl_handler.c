/*
 * (C) Copyright 2019
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <linux/version.h>
#include <sys/ioctl.h>
#include <stddef.h>

#include <mtd/mtd-user.h>
#include "swupdate_image.h"
#include "handler.h"
#include "util.h"
#include "flash.h"

#define PATH_TO_MTD	"/dev/mtd"

/*
 * This is used to write a general function
 * to parse the properties and passing the type
 */

typedef enum {
	INT32_TYPE,
	STRING_TYPE
} PROPTYPE;

/*
 * The administration sector
 * There are two copies of admin
 */
struct ssbl_admin_sector {
	uint32_t magic_age;
	uint32_t image_offs;
	uint32_t image_size;
};

struct ssbl_priv {
	char device[MAX_VOLNAME];
	int32_t admin_offs;
	uint32_t image_offs;
	uint32_t image_size;
	int mtdnum;
	struct ssbl_admin_sector ssbl;
};

struct proplist {
	const char *name;
	PROPTYPE type;
	size_t offset;
};

static struct proplist list[] =  {
	{"device", STRING_TYPE, offsetof(struct ssbl_priv, device)},
	{"offset", INT32_TYPE, offsetof(struct ssbl_priv, admin_offs)},
	{"imageoffs", INT32_TYPE, offsetof(struct ssbl_priv, image_offs)},
	{"imagesize", INT32_TYPE, offsetof(struct ssbl_priv, image_size)},
	{ NULL, INT32_TYPE,  0 }
};

/*
 * Mask for magic_age
 */
#define SSBL_MAGIC	0x1CEEDBEE
#define get_ssbl_age(t) ((t & 0x07) % 3)
#define get_ssbl_magic(t) ((t & ~0x07) >> 3)

void ssbl_handler(void);

static inline bool ssbl_verify_magic(struct ssbl_priv *adm)
{
	return get_ssbl_magic(adm->ssbl.magic_age) == SSBL_MAGIC;
}
static inline int ssbl_get_age(struct ssbl_priv *adm)
{
	return get_ssbl_age(adm->ssbl.magic_age);
}

static bool ssbl_retrieve_property(struct img_type *img, const char *name,
				   struct ssbl_priv *admins,
				   uint32_t offset, PROPTYPE type)
{
	struct dict_list *proplist;
	struct dict_list_elem *property;
	int num = 0;
	uint32_t *pval;
	char *p;

	proplist = dict_get_list(&img->properties, name);

	if (!proplist)
		return false;

	LIST_FOREACH(property, proplist, next) {
		if (num >= 2) {
			ERROR("SSBL switches between two structures, too many found (%s)", name);
			return false;
		}

		switch (type) {
		case INT32_TYPE:
			pval = (uint32_t *) ((size_t)&admins[num] + offset);
			*pval = strtoul(property->value, NULL, 0);
			break;
		case STRING_TYPE:
			p = (char *)((size_t)&admins[num] + offset);
			strncpy(p, property->value, MAX_VOLNAME);
			break;
		}
		num++;
	}

	if (num != 2)
		return false;

	return true;
}

/*
 * Check which SSBL is the standby copy
 * At least one of the two SSBL Admin block
 * must contain valid data
 */
static int get_inactive_ssbl(struct ssbl_priv *padmins)
{
	int i;
	int age0, age1;

	for (i = 0; i < 2; i++) {
		if (!ssbl_verify_magic(&padmins[i]))
			return i;
	}

	/*
	 * Both valid, check age
	 */
	age0 = ssbl_get_age(&padmins[0]);
	age1 = ssbl_get_age(&padmins[1]);

	DEBUG("AGES : %s --> %d %s-->%d",
		padmins[0].device, age0,
		padmins[1].device, age1);

	if (!age0 && age1 == 2)
		age0 = 3;
	if (!age1 && age0 == 2)
		age1 = 3;

	if (age1 > age0)
		return 0;

	return 1;
}

static int inline get_active_ssbl(struct ssbl_priv *padmins) {
	return get_inactive_ssbl(padmins) == 1 ? 0 : 1;
}

static int ssbl_swap(struct img_type *img, void *data)
{
	struct script_handler_data *script_data;
	struct ssbl_priv admins[2];
	struct ssbl_priv *pssbl;
	struct proplist *entry;
	int iter, ret;
	int fd;
	struct flash_description *flash = get_flash_info();
	char mtd_device[80];

	if (!data)
		return -EINVAL;

	script_data = data;

	/*
	 * Call only in case of postinstall
	 */
	if (script_data->scriptfn != POSTINSTALL)
		return 0;

	memset(admins, 0, 2 * sizeof(struct ssbl_priv));

	entry = &list[0];
	while (entry->name) {
		if (!ssbl_retrieve_property(img, entry->name, admins,
					    entry->offset, entry->type)) {
			ERROR("Cannot get %s from sw-description", entry->name);
			return -EINVAL;
		}
		entry++;
	}

	/*
	 * Retrieve SSBL Admin sectors
	 */
	for (iter = 0; iter < 2; iter++) {
		pssbl = &admins[iter];
		pssbl->mtdnum = get_mtd_from_device(pssbl->device);
		if (pssbl->mtdnum < 0) {
		/* Allow device to be specified by name OR number */
			pssbl->mtdnum = get_mtd_from_name(pssbl->device);
		}
		if (pssbl->mtdnum < 0 || !mtd_dev_present(flash->libmtd,
							  pssbl->mtdnum)) {
			ERROR("%s does not exist: partitioning not possible",
			pssbl->device);
			return -ENODEV;
		}
		snprintf(mtd_device, sizeof(mtd_device),
			"%s%d", PATH_TO_MTD, pssbl->mtdnum);
		if ((fd = open(mtd_device, O_RDWR)) < 0) {
			ERROR( "%s: %s: %s", __func__, mtd_device,
				strerror(errno));
			return -ENODEV;
		}

		ret = read(fd, &pssbl->ssbl, sizeof(struct ssbl_admin_sector));
		close(fd);
		if (ret < 0) {
			ERROR("%s: SSBL cannot be read: %s", mtd_device,
			       strerror(errno));
			return -ENODEV;
		}
	}

	/*
	 * Perform the switch:
	 * - find the inactive copy
	 * - increment age
	 * - write to flash
	 */
	pssbl = &admins[get_inactive_ssbl(admins)];
	flash_erase(pssbl->mtdnum);	/* erase inactive copy */
	snprintf(mtd_device, sizeof(mtd_device), "%s%d", PATH_TO_MTD,
		 pssbl->mtdnum);
	pssbl->ssbl.image_size = pssbl->image_size;
	pssbl->ssbl.image_offs = pssbl->image_offs;

	/*
	 * Get age from the active copy and increment it
	 * age is module 3
	 */
	pssbl->ssbl.magic_age = 0xFFFFFFF8 |
		((ssbl_get_age(&admins[get_active_ssbl(admins)]) + 1) % 3);

	/* Write to flash */
	if ((fd = open(mtd_device, O_RDWR)) < 0) {
		ERROR( "%s: %s: %s", __func__, mtd_device, strerror(errno));
		return -ENODEV;
	}
	ret = write(fd, &pssbl->ssbl, sizeof(struct ssbl_admin_sector));
	if (ret != sizeof(struct ssbl_admin_sector)) {
		ERROR( "Cannot write SSBL admin : %s: %s", mtd_device,
			strerror(errno));
		close(fd);
		return -EIO;
	}

	/* Last but not least, write the magic to make the SSBL valid */
	pssbl->ssbl.magic_age = (pssbl->ssbl.magic_age & 0x07) | (SSBL_MAGIC << 3);

	/* Magic is at the beginning of sector */
	lseek(fd, 0, SEEK_SET);
	ret = write(fd, &pssbl->ssbl.magic_age, sizeof(uint32_t));
	close(fd);
	if (ret != sizeof(uint32_t)) {
		ERROR( "Cannot write SSBL admin : %s: %s", mtd_device,
			strerror(errno));
		close(fd);
		return -EIO;
	}

	return 0;
}

__attribute__((constructor))
void ssbl_handler(void)
{
	register_handler("ssblswitch", ssbl_swap,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}
