/*
 * (C) Copyright 2023
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "util.h"
#include "hw-compatibility.h"

/*
 * The HW revision of the board *MUST* be inserted
 * in the sw-description file
 */
#ifdef CONFIG_HW_COMPATIBILITY
int check_hw_compatibility(struct hw_type *hwt, struct hwlist *hardware)
{
	struct hw_type *hw;
	int ret;

	ret = get_hw_revision(hwt);
	if (ret < 0)
		return -1;

	TRACE("Hardware %s Revision: %s", hwt->boardname, hwt->revision);
	LIST_FOREACH(hw, hardware, next) {
		if (hw &&
		    (!hwid_match(hw->revision, hwt->revision))) {
			TRACE("Hardware compatibility verified");
			return 0;
		}
	}

	return -1;
}
#else
int check_hw_compatibility(struct hw_type __attribute__ ((__unused__)) *hwt, struct hwlist __attribute__ ((__unused__)) *hardware)
{
	return 0;
}
#endif

/*
 * This function is strict bounded with the hardware
 * It reads some GPIOs to get the hardware revision
 */
int get_hw_revision(struct hw_type *hw)
{
	FILE *fp;
	int ret;
	char *b1, *b2;
#ifdef CONFIG_HW_COMPATIBILITY_FILE
#define HW_FILE CONFIG_HW_COMPATIBILITY_FILE
#else
#define HW_FILE "/etc/hwrevision"
#endif

	if (!hw)
		return -EINVAL;

	/*
	 * do not overwrite if it is already set
	 * (maybe from command line)
	 */
	if (strlen(hw->boardname))
		return 0;

	memset(hw->boardname, 0, sizeof(hw->boardname));
	memset(hw->revision, 0, sizeof(hw->revision));

	/*
	 * Not all boards have pins for revision number
	 * check if there is a file containing theHW revision number
	 */
	fp = fopen(HW_FILE, "r");
	if (!fp)
		return -1;

	ret = fscanf(fp, "%ms %ms", &b1, &b2);
	fclose(fp);

	if (ret != 2) {
		TRACE("Cannot find Board Revision");
		if(ret == 1)
			free(b1);
		return -1;
	}

	if ((strlen(b1) > (SWUPDATE_GENERAL_STRING_SIZE) - 1) ||
		(strlen(b2) > (SWUPDATE_GENERAL_STRING_SIZE - 1))) {
		ERROR("Board name or revision too long");
		ret = -1;
		goto out;
	}

	strlcpy(hw->boardname, b1, sizeof(hw->boardname));
	strlcpy(hw->revision, b2, sizeof(hw->revision));

	ret = 0;

out:
	free(b1);
	free(b2);

	return ret;
}


