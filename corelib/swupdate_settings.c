/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * This allows to have a configuration file instead of
 * starting swupdate with a long list of parameters.
 */

#include <libconfig.h>
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
#include "swupdate_dict.h"
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

	if (!filename)
		return -EINVAL;

	DEBUG("Reading config file %s", filename);
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

int read_module_settings(swupdate_cfg_handle *handle, const char *module, settings_callback fcn, void *data)
{
	config_setting_t *elem;

	if (handle == NULL || !fcn)
		return -EINVAL;

	elem = find_settings_node(&handle->cfg, module);

	if (!elem) {
		DEBUG("No config settings found for module %s", module);
		return -ENODATA;
	}

	DEBUG("Reading config settings for module %s", module);
	fcn(elem, data);

	return 0;
}

static int get_run_as(void *elem, void *data)
{
	struct run_as *pid = (struct run_as *)data;

	get_field(LIBCFG_PARSER, elem, "userid", &pid->userid);
	get_field(LIBCFG_PARSER, elem, "groupid", &pid->groupid);

	return 0;
}

int read_settings_user_id(swupdate_cfg_handle *handle, const char *module, uid_t *userid, gid_t *groupid)
{
	struct run_as ids;
	int ret;

	*userid = ids.userid = getuid();
	*groupid = ids.groupid = getgid();

	ret = read_module_settings(handle, module, get_run_as, &ids);
	if (ret)
		return -EINVAL;

	*userid = ids.userid;
	*groupid = ids.groupid;

	return 0;
}


/*
 * Callback to be used to put a section of configuration
 * into a dictionary
 */
int settings_into_dict(void *settings, void *data)
{
	void *elem;
	int count, i;
	char name[80], value[80];
	struct dict *dictionary = (struct dict *) data;

	count = get_array_length(LIBCFG_PARSER, settings);

	for(i = 0; i < count; ++i) {
		elem = get_elem_from_idx(LIBCFG_PARSER, settings, i);

		if (!elem)
			continue;

		if(!(exist_field_string(LIBCFG_PARSER, elem, "name")))
			continue;
		if(!(exist_field_string(LIBCFG_PARSER, elem, "value")))
			continue;

		GET_FIELD_STRING(LIBCFG_PARSER, elem, "name", name);
		GET_FIELD_STRING(LIBCFG_PARSER, elem, "value", value);
		dict_set_value(dictionary, name, value);
		TRACE("Identify for configData: %s --> %s\n",
				name, value);
	}

	return 0;
}

/*
 * Initialize handle with the settings found in filename.
 * This allocates memory which needs to be released by calling swupdate_cfg_destroy().
 */
void swupdate_cfg_init(swupdate_cfg_handle *handle)
{
	config_init(&handle->cfg);
}

/*
 * Read all settings from filename.
 */
int swupdate_cfg_read_file(swupdate_cfg_handle *handle, const char *filename)
{
	if (read_settings_file(&handle->cfg, filename) != CONFIG_TRUE) {
		ERROR("Error reading configuration file %s", filename);
		return -EINVAL;
	}
	return 0;
}

/*
 * This releases (internally) allocated memory by handle.
 */
void swupdate_cfg_destroy(swupdate_cfg_handle *handle)
{
	config_destroy(&handle->cfg);
}
