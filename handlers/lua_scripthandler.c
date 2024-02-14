/*
 * (C) Copyright 2013-2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <sys/types.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "swupdate_image.h"
#include "handler.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "util.h"
#include "lua_util.h"

static void lua_handler(void);

static int start_lua_script(struct img_type *img, void *data)
{
	int ret;
	const char *fnname;
	struct script_handler_data *script_data;
	lua_State *L;
	const char* tmp = get_tmpdirscripts();
	char filename[MAX_IMAGE_FNAME + strlen(tmp) + 2 + strlen(img->type_data)];

	if (!data)
		return -1;

	bool global  = strtobool(dict_get_value(&img->properties, "global-state"));

	if (global) {
		TRACE("Executing with global state");
		L = img->L;
	} else {
		L = lua_init(img->bootloader);
	}

	if (!L) {
		ERROR("Lua state cannot be instantiated");
		return -1;
	}

	script_data = data;

	switch (script_data->scriptfn) {
	case PREINSTALL:
		fnname = dict_get_value(&img->properties, "preinstall");
		if (!fnname)
			fnname="preinst";
		break;
	case POSTINSTALL:
		fnname = dict_get_value(&img->properties, "postinstall");
		if (!fnname)
			fnname="postinst";
		break;
	case POSTFAILURE:
		fnname = dict_get_value(&img->properties, "postfailure");
		if (!fnname)
			fnname="postfailure";
		break;
	default:
		/* no error, simply no call */
		return 0;
	}

	snprintf(filename, sizeof(filename),
		"%s%s", tmp, img->fname);
	TRACE("Calling Lua %s with %s", filename, fnname);

	ret = run_lua_script(L, filename, fnname, img->type_data);

	if (!global)
		lua_close(L);

	return ret;
}

 __attribute__((constructor))
static void lua_handler(void)
{
	register_handler("lua", start_lua_script, SCRIPT_HANDLER, NULL);
}
