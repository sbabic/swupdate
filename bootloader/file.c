/*
 * (C) Copyright 2017
 * Cedric Hombourger, Mentor Graphics, Cedric_Hombourger@mentor.com
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

#include <errno.h>
#include <unistd.h>
#include <libconfig.h>

#include "bootloader.h"
#include "util.h"

int bootloader_env_set(const char *name, const char *value)
{
	config_t cfg;
	config_setting_t *root, *setting;
	int result = CONFIG_FALSE;

	config_init(&cfg);
	(void) config_read_file(&cfg, CONFIG_BOOTLOADER_FILE_PATH);
	setting = config_lookup(&cfg, name);

	/* Check if setting already exists */
	if (setting) {
		result = config_setting_set_string(setting, value);
	}
	else {
		/* Otherwise add... */
		root = config_root_setting(&cfg);
		setting = config_setting_add(root, name, CONFIG_TYPE_STRING);
		if (setting) {
			result = config_setting_set_string(setting, value);
		}
		else result = CONFIG_FALSE;
	}

	/* Write configuration */
	if (result == CONFIG_TRUE) {
		result = config_write_file(&cfg, CONFIG_BOOTLOADER_FILE_PATH);
	}

	config_destroy(&cfg);
	result = (result == CONFIG_TRUE) ? 0 : -1;
	return result;
}

int bootloader_env_unset(const char *name)
{
	config_t cfg;
	config_setting_t *root;
	int result = CONFIG_FALSE;

	config_init(&cfg);
	(void) config_read_file(&cfg, CONFIG_BOOTLOADER_FILE_PATH);

	/* Try to remove the requested setting */
	root = config_root_setting(&cfg);
	result = config_setting_remove(root, name);

	/* Write configuration */
	if (result == CONFIG_TRUE) {
		result = config_write_file(&cfg, CONFIG_BOOTLOADER_FILE_PATH);
	}
	else result = CONFIG_TRUE;

	config_destroy(&cfg);

	result = (result == CONFIG_TRUE) ? 0 : -1;
	return result;
}

char *bootloader_env_get(const char *name)
{
	config_t cfg;
	const char *value = NULL;

	config_init(&cfg);
	if (config_read_file(&cfg, CONFIG_BOOTLOADER_FILE_PATH) == CONFIG_TRUE) {
		if (config_lookup_string(&cfg, name, &value) == CONFIG_TRUE) {
			value = strdup(value);
		}
	}
	config_destroy(&cfg);
	return (char *)value;
}

int bootloader_apply_list(const char *filename)
{
	config_t cfg;
	int result;

	config_init(&cfg);
	result = config_read_file(&cfg, filename);
        if (result == CONFIG_TRUE) {
		result = config_write_file(&cfg, CONFIG_BOOTLOADER_FILE_PATH);
        }

	result = (result == CONFIG_TRUE) ? 0 : -1;
	return result;
}
