/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
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

#include "swupdate.h"
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
	script_fn scriptfn;

	const char* tmp = get_tmpdirscripts();
	char filename[MAX_IMAGE_FNAME + strlen(tmp) + 2 + strlen(img->type_data)];

	if (!data)
		return -1;

	scriptfn = *(script_fn *)data;

	switch (scriptfn) {
	case PREINSTALL:
		fnname="preinst";
		break;
	case POSTINSTALL:
		fnname="postinst";
		break;
	default:
		/* no error, simply no call */
		return 0;
	}

	snprintf(filename, sizeof(filename),
		"%s%s", tmp, img->fname);
	TRACE("Calling Lua %s", filename);

	ret = run_lua_script(filename, fnname, img->type_data);

	return ret;

}

 __attribute__((constructor))
static void lua_handler(void)
{
	register_handler("lua", start_lua_script, SCRIPT_HANDLER, NULL);
}
