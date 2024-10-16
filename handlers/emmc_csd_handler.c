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
#if defined(__linux__)
#include <linux/version.h>
#endif
#include <sys/ioctl.h>
#if defined(__linux__)
#include <linux/mmc/ioctl.h>
#elif defined(__FreeBSD__)
#include <dev/mmc/mmc_ioctl.h>
#endif
#include "swupdate_image.h"
#include "handler.h"
#include "util.h"

void emmc_csd_handler(void);
void emmc_csd_toggle_handler(void);


/*
 * This set the CSD register (boot device) on eMMC
 * The device to be set is passed as argument if toggle is fale
 * else the booting device is detected and the function toggles
 * the boot device
 */
static int emmc_csd_set(struct img_type *img, void *data, bool toggle)
{
	int active, ret;
	struct script_handler_data *script_data = data;
	char tmpdev[SWUPDATE_GENERAL_STRING_SIZE];

	if (!script_data)
		return -EINVAL;

	if (script_data->scriptfn == PREINSTALL)
		return 0;
	
	strlcpy(tmpdev, img->device, sizeof(tmpdev));

	if (!toggle) {
		char *boot = strstr(tmpdev, "boot");
		if (!boot) {
			ERROR("The boot device as mmcblkXboot[0|1] must be set");
			return -EINVAL;
		}
		if (strlen(boot) < 5) {
			ERROR("The value for boot device is not set");
			return -EINVAL;
		}
		active = strtoul(&boot[4], NULL, 10);
		if (errno || (active != 0 && active != 1)) {
			ERROR("Wrong boot device set: %s", img->device);

		}
		*boot = '\0';
	}

	/* Open the device */
	int fdin = open(tmpdev, O_RDONLY);
	if (fdin < 0) {
		ERROR("Failed to open %s: %s", tmpdev, strerror(errno));
		return -ENODEV;
	}

	if (toggle) {
		active = emmc_get_active_bootpart(fdin);
		if (active < 0) {
			ERROR("Current HW boot partition cannot be retrieved");
			close(fdin);
			return -ENODEV;
		}
		if (active > 1)
			WARN("Boot device set to User area, no changes !");
		else
			active = (active == 0) ? 1 : 0;

	}

	/*
	 * If User Partition is activated, does nothing
	 * and report this to the user.
	 */
	ret = 0;
	if (active == 0 || active == 1) {
		TRACE("Setting Boot to HW Partition %d", active);
		ret = emmc_write_bootpart(fdin, active);
		if (ret)
			ERROR("Failure writing CSD register");
	}

	close(fdin);
	return ret;
}

static int emmc_boot(struct img_type *img, void *data)
{
	return emmc_csd_set(img, data, false);
}

static int emmc_boot_toggle(struct img_type *img, void *data)
{
	return emmc_csd_set(img, data, true);
}

__attribute__((constructor))
void emmc_csd_toggle_handler(void)
{
	register_handler("emmc_boot_toggle", emmc_boot_toggle,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}

__attribute__((constructor))
void emmc_csd_handler(void)
{
	register_handler("emmc_boot", emmc_boot,
				SCRIPT_HANDLER | NO_DATA_HANDLER, NULL);
}
