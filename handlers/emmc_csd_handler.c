/*
 * (C) Copyright 2024
 * Stefano Babic, stefano.babic@swupdate.org
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */
/*
 * This handler manages the CSD register accoriding to eMMC
 * specifications. Base for this handler are the mmcutils,
 * see:
 *     https://git.kernel.org/pub/scm/utils/mmc/mmc-utils.git
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <linux/version.h>
#include <sys/ioctl.h>
#include <linux/mmc/ioctl.h>
#include "swupdate_image.h"
#include "handler.h"
#include "util.h"

void emmc_csd_handler(void);

static int emmc_boot_toggle(struct img_type *img, void *data)
{
	int active, ret;
	struct script_handler_data *script_data = data;
	if (script_data->scriptfn == PREINSTALL)
		return 0;

	/* Open the device (partition) */
	int fdin = open(img->device, O_RDONLY);
	if (fdin < 0) {
		ERROR("Failed to open %s: %s", img->device, strerror(errno));
		return -ENODEV;
	}

	active = emmc_get_active_bootpart(fdin);
	if (active < 0) {
		ERROR("Current HW boot partition cannot be retrieved");
		close(fdin);
		return -1;
	}

	/*
	 * If User Partition is activated, does nothing
	 * and report this to the user.
	 */
	if (active > 1) {
		WARN("Boot device set to User area, no changes !");
		ret = 0;
	} else {
		active = (active == 0) ? 1 : 0;
		TRACE("Setting Boot to HW Partition %d", active);
		ret = emmc_write_bootpart(fdin, active);
		if (ret)
			ERROR("Failure writing CSD register");
	}

	close(fdin);
	return ret;
}

__attribute__((constructor))
void emmc_csd_handler(void)
{
	register_handler("emmc_boot_toggle", emmc_boot_toggle,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}
