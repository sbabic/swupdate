/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
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
#include "swupdate_settings.h"
#include "compat.h"

struct run_as {
	uid_t userid;
	gid_t groupid;
};

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

int read_module_settings(const char *filename, const char *module, settings_callback fcn, void *data)
{
	config_t cfg;
	config_setting_t *elem;

	if (!fcn || !filename)
		return -EINVAL;

	memset(&cfg, 0, sizeof(cfg));
	config_init(&cfg);

	/* Read the file. If there is an error, report it and exit. */
	if (read_settings_file(&cfg, filename) != CONFIG_TRUE) {
		config_destroy(&cfg);
		ERROR("Error reading configuration file, skipping....");
		return -EINVAL;
	}

	elem = find_settings_node(&cfg, module);

	if (!elem) {
		config_destroy(&cfg);
		return -ENODATA;
	}

	fcn(elem, data);

	config_destroy(&cfg);

	return 0;
}

static int get_run_as(void *elem, void *data)
{
	struct run_as *pid = (struct run_as *)data;

	get_field(LIBCFG_PARSER, elem, "userid", &pid->userid);
	get_field(LIBCFG_PARSER, elem, "groupid", &pid->groupid);

	return 0;
}

int read_settings_user_id(const char *filename, const char *module, uid_t *userid, gid_t *groupid)
{
	struct run_as ids;
	int ret;

	*userid = ids.userid = getuid();
	*groupid = ids.groupid = getgid();

	ret = read_module_settings(filename, module, get_run_as, &ids);
	if (ret)
		return -EINVAL;

	*userid = ids.userid;
	*groupid = ids.groupid;

	return 0;
}
