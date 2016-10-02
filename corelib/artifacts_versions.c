/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
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
			strncpy(swcomp->name, name, sizeof(swcomp->name));
			strncpy(swcomp->version, version, sizeof(swcomp->version));
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
