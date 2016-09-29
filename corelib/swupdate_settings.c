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
 * Foundation, Inc.
 */

/*
 * This allows to have a configuration file instead of
 * starting swupdate with a long list of parameters.
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

static config_setting_t *find_settings_node(config_t *cfg,
						const char *field)
{
	char node[1024];
	config_setting_t *setting;

	if (!field)
		return NULL;

	snprintf(node, sizeof(node), "%s", field);

	setting = config_lookup(cfg, node);

	return setting;
}

static int read_settings_file(config_t *cfg, const char *filename)
{
	int ret;

	/* Read the file. If there is an error, report it and exit. */
	ret = config_read_file(cfg, filename);
	if (ret != CONFIG_TRUE) {
		fprintf(stderr, "%s ", config_error_file(cfg));
		fprintf(stderr, "%d ", config_error_line(cfg));
		fprintf(stderr, "%s ", config_error_text(cfg));

		fprintf(stderr, "%s:%d - %s\n", config_error_file(cfg),
			config_error_line(cfg), config_error_text(cfg));
	}

	return ret;
}

int read_module_settings(char *filename, const char *module, settings_callback fcn, void *data)
{
	config_t cfg;
	config_setting_t *elem;

	if (!fcn)
		return -EINVAL;

	memset(&cfg, 0, sizeof(cfg));
	config_init(&cfg);

	/* Read the file. If there is an error, report it and exit. */
	if (read_settings_file(&cfg, filename) != CONFIG_TRUE) {
		config_destroy(&cfg);
		ERROR("Error reading configuration file, skipping....\n");
		return -EINVAL;
	}

	elem = find_settings_node(&cfg, module);

	if (!elem) {
		config_destroy(&cfg);
		return -EINVAL;
	}

	fcn(elem, data);

	config_destroy(&cfg);

	return 0;
}
