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

struct fn_names {
	const char *property_name;	/* Name of property in sw-description */
	const char *def_fn;		/* Default name function if property is not set */
};

const struct fn_names fn_property_names[] = {
	[PREINSTALL] = {"preinstall", "preinst"},
	[POSTINSTALL] = {"postinstall", "postinst"},
	[POSTFAILURE] = {"postfailure", "postfailure"}
};

static int start_lua_script(struct img_type *img, void *data)
{
	int ret;
	const char *fnname = NULL;
	struct script_handler_data *script_data;
	lua_State *L;
	const char* tmp = get_tmpdirscripts();
	char filename[MAX_IMAGE_FNAME + strlen(tmp) + 2 + strlen(img->type_data)];

	if (!data)
		return -1;

	script_data = data;

	/*
	 * A little paranoid, thios shouln't happen
	 */
	if (script_data->scriptfn < PREINSTALL || script_data->scriptfn > POSTFAILURE)
		return -EINVAL;

	bool global  = strtobool(dict_get_value(&img->properties, "global-state"));

	/*
	 * Note: if global is set, functions should be unique
	 * The name of function should be set inside the script
	 */
	fnname = dict_get_value(&img->properties, fn_property_names[script_data->scriptfn].property_name);

	if (!fnname && !global) {
		fnname = fn_property_names[script_data->scriptfn].def_fn;
	}

	/*
	 * Assign the Lua state
	 */
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

	/*
	 * In case of global, loads the script just once. Check if a function is already
	 * present. An Overwrite is excluded by design. All functions should be unique.
	 */

	bool load_script = true;

	if (global) {
		if (script_data->scriptfn == PREINSTALL) {
			for (int i = PREINSTALL; i <= POSTINSTALL; i++) {
				const char *fn = fn_property_names[script_data->scriptfn].property_name;
				if (!fn)
					continue;
				lua_getglobal(L, fn);
				if(lua_isfunction(L,lua_gettop(L))) {
					ERROR("Function %s already defined, functions must be unique", fn);
					return -1;
				}
			}
		} else {
			/*
			 * Script was already loaded, skip it
			 */
			load_script = false;
		}
	}

	/*
	 * Trace what should be done
	 */
	snprintf(filename, sizeof(filename),
		"%s%s", tmp, img->fname);
	TRACE("%s: Calling Lua %s with %s",
	      fn_property_names[script_data->scriptfn].property_name,
	      filename,
	      fnname ? fnname : "no function, just loaded");

	/*
	 * In case no function is selected and we run in global,
	 * script was already loaded and there is nothing to do
	 */
	if (global && !fnname && !load_script)
		return 0;

	ret = run_lua_script(L, filename, load_script, fnname, img->type_data);

	if (!global)
		lua_close(L);

	return ret;
}

 __attribute__((constructor))
static void lua_handler(void)
{
	register_handler("lua", start_lua_script, SCRIPT_HANDLER, NULL);
}
