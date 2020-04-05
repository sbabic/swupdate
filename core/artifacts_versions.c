/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <linux/types.h>
#endif
#include <compat.h>
#include <limits.h>
#include <assert.h>
#include "generated/autoconf.h"
#include "bsdqueue.h"
#include "util.h"
#include "swupdate.h"
#include "parselib.h"
#include "swupdate_settings.h"

/*
 * Read versions of components from a file, if provided
 * This is used to check for version mismatch and avoid
 * to reinstall a component that is already installed
 */
#ifdef CONFIG_SW_VERSIONS_FILE
#define SW_VERSIONS_FILE CONFIG_SW_VERSIONS_FILE
#else
#define SW_VERSIONS_FILE "/etc/sw-versions"
#endif
static int read_sw_version_file(struct swupdate_cfg *sw)
{
	FILE *fp;
	int ret;
	char *name, *version;
	struct sw_version *swcomp;

	/*
	 * scan all entries inside SW_VERSIONS_FILE
	 * and generate a list
	 */

	fp = fopen(SW_VERSIONS_FILE, "r");
	if (!fp)
		return -EACCES;

	while (1) {
		ret = fscanf(fp, "%ms %ms", &name, &version);
		/* pair component / version found */
		if (ret == 2) {
			swcomp = (struct sw_version *)calloc(1, sizeof(struct sw_version));
			if (!swcomp) {
				ERROR("Allocation error");
				return -ENOMEM;
			}
			strlcpy(swcomp->name, name, sizeof(swcomp->name));
			strlcpy(swcomp->version, version, sizeof(swcomp->version));
			LIST_INSERT_HEAD(&sw->installed_sw_list, swcomp, next);
			TRACE("Installed %s: Version %s",
					swcomp->name,
					swcomp->version);
			free(name);
			free(version);
		} else {
			if (ret == EOF)
				break;
			if (errno) {
				ERROR("Malformed sw-versions file, skipped !");
				break;
			}

			/*
			 * Malformed file, skip the line
			 * and check next
			 */
			if (ret == 1)
				free(name);
		}
	}
	fclose(fp);

	return 0;
}

#ifdef CONFIG_LIBCONFIG
static int versions_settings(void *setting, void *data)
{
	struct swupdate_cfg *sw = (struct swupdate_cfg *)data;
	void *elem;
	int count, i;
	struct sw_version *swcomp;

	count = get_array_length(LIBCFG_PARSER, setting);

	for(i = 0; i < count; ++i) {
		elem = get_elem_from_idx(LIBCFG_PARSER, setting, i);

		if (!elem)
			continue;

		swcomp = (struct sw_version *)calloc(1, sizeof(struct sw_version));
		if (!swcomp) {
			ERROR("Allocation error");
			return -ENOMEM;
		}

		GET_FIELD_STRING(LIBCFG_PARSER, elem, "name", swcomp->name);
		GET_FIELD_STRING(LIBCFG_PARSER, elem, "version", swcomp->version);

		LIST_INSERT_HEAD(&sw->installed_sw_list, swcomp, next);
		TRACE("Installed %s: Version %s",
			swcomp->name,
			swcomp->version);
	}

	return 0;
}

void get_sw_versions(char *cfgname, struct swupdate_cfg *sw)
{
	int ret = -EINVAL;

	/*
	 * Try to read versions from configuration file
	 * If not found, fall back to a legacy file
	 * in the format "<image name> <version>"
	 */
	if (cfgname)
		ret = read_module_settings(cfgname, "versions",
						versions_settings,
						sw);

	if (ret)
		ret = read_sw_version_file(sw);

}
#else

void get_sw_versions(char __attribute__ ((__unused__)) *cfgname,
			struct swupdate_cfg *sw)
{
	read_sw_version_file(sw);
}
#endif

/*
 * convert a version string into a number
 * version string is in the format:
 *
 * 	major.minor.revision.buildinfo
 *
 * but they do not need to have all fields.
 * Also major.minor or major.minor.revision are allowed
 * The conversion genearets a 64 bit value that can be compared
 */
__u64 version_to_number(const char *version_string)
{
	char **versions = NULL;
	char **ver;
	unsigned int count = 0;
	__u64 version = 0;

	versions = string_split(version_string, '.');
	for (ver = versions; *ver != NULL; ver++, count ++) {
		if (count < 4) {
			unsigned long int fld = strtoul(*ver, NULL, 10);
			/* check for return of strtoul, mandatory */
			if (fld != ULONG_MAX) {
				fld &= 0xffff;
				version = (version << 16) | fld;
			}
		}
		free(*ver);
	}
	if (count < 4)
		version <<= 16 * (4 - count);
	free(versions);

	return version;
}
