/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
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
	char filename[MAX_IMAGE_FNAME + strlen(tmp) + 2];

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

	ret = run_lua_script(filename, fnname, filename);

	return ret;

}

 __attribute__((constructor))
static void lua_handler(void)
{
	register_handler("lua", start_lua_script, SCRIPT_HANDLER, NULL);
}
