/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "generated/autoconf.h"
#include "swupdate.h"
#include "swupdate_dict.h"
#include "handler.h"
#include "util.h"
#include "bootloader.h"
#include "progress.h"

static void uboot_handler(void);
static void boot_handler(void);

static int install_boot_environment(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	int ret;
	int fdout;
	FILE *fp;
	char *buf;
	char filename[MAX_IMAGE_FNAME];
	struct stat statbuf;

	if (snprintf(filename, sizeof(filename), "%s%s", get_tmpdirscripts(),
		     img->fname) >= (int)sizeof(filename)) {
		ERROR("Path too long: %s%s", get_tmpdirscripts(),
			 img->fname);
		return -1;
	}

	if (!img->bootloader) {
		ERROR("Internal fault, please report !");
		return -EFAULT;
	}
	ret = stat(filename, &statbuf);
	if (ret) {
		fdout = openfileoutput(filename);
		ret = copyimage(&fdout, img, NULL);
		close(fdout);
	}

	/*
	 * Do not set now the bootloader environment
	 * because this can cause a corrupted environment
	 * if a successive handler fails.
	 * Just put the environment in a the global list
	 * and let that the core will update it at the end
	 * of install
	 */
	fp = fopen(filename, "r");
	if (!fp)
		return -EACCES;

	buf = (char *)malloc(MAX_BOOT_SCRIPT_LINE_LENGTH);
	if (!buf) {
		ERROR("Out of memory, exiting !");
		fclose(fp);
		return -ENOMEM;
	}

	while (fgets(buf, MAX_BOOT_SCRIPT_LINE_LENGTH, fp)) {
		char **pair = NULL;
		unsigned int cnt;
		int len = strlen(buf);

		while (len && (buf[len - 1] == '\n' || buf [len - 1] == '\r'))
			buf[--len] = '\0';

		/* Skip comment or empty lines */
		if (len == 0 || buf[0] == '#')
			continue;

		pair = string_split(buf, '=');
		cnt = count_string_array((const char **)pair);

		switch (cnt) {
		case 2:
			TRACE("name = %s value = %s", pair[0], pair[1]);
			dict_set_value(img->bootloader, pair[0], pair[1]);
			break;
		case 1:
			TRACE("name = %s Removed", pair[0]);
			dict_set_value(img->bootloader, pair[0], "");
			break;
		default:
			/*
			 * If value contains "=", splitargs returns
			 * more substrings. Then pairs[1]..pairs[N]
			 * should be treated as single string for
			 * the dictionary
			 */
			if (cnt > 2)  {
				char *tmp = strchr(buf, '=');
				if (tmp && ((tmp - buf) < (len - 1))) {
					tmp++;
					TRACE("name = %s value = %s", pair[0], tmp);
					dict_set_value(img->bootloader, pair[0], tmp);
				}
			}
		}
		free(pair);
	}
	/*
	 * this handler does not use copyfile()
	 * and must update itself the progress bar
	 */
	swupdate_progress_update(100);

	fclose(fp);
	free(buf);

	return 0;
}

__attribute__((constructor))
static void uboot_handler(void)
{
	register_handler("uboot", install_boot_environment,
				IMAGE_HANDLER | BOOTLOADER_HANDLER, NULL);
}
__attribute__((constructor))
static void boot_handler(void)
{
	register_handler("bootloader", install_boot_environment,
				IMAGE_HANDLER | BOOTLOADER_HANDLER, NULL);
}
