/*
 * (C) Copyright 2017
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <unistd.h>
#include <string.h>
#include "bootloader.h"
#include "swupdate_dict.h"

static struct dict environment;

static int do_env_set(const char *name,
			const char  *value)
{
	dict_set_value(&environment, name, value);

	return 0;
}

static int do_env_unset(const char *name)
{
	dict_remove(&environment, name);

	return 0;
}

static char *do_env_get(const char  *name)
{
	char *value = NULL, *var;

	var = dict_get_value(&environment, name);

	if (var)
		value = strdup(var);

	return value;
}

static int do_apply_list(const char *filename)
{
	return dict_parse_script(&environment, filename);
}

static bootloader none = {
	.env_get = &do_env_get,
	.env_set = &do_env_set,
	.env_unset = &do_env_unset,
	.apply_list = &do_apply_list
};

__attribute__((constructor))
static void none_probe(void)
{
	(void)register_bootloader(BOOTLOADER_NONE, &none);
}
