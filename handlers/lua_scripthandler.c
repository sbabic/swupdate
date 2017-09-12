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
	const char *output;
	script_fn scriptfn;
	lua_State *L = luaL_newstate(); /* opens Lua */
	const char* TMPDIR = get_tmpdir();
	char filename[MAX_IMAGE_FNAME + strlen(TMPDIR) +
		strlen(SCRIPTS_DIR_SUFFIX) + 2];

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
		"%s%s%s", TMPDIR, SCRIPTS_DIR_SUFFIX, img->fname);
	TRACE("Calling Lua %s", filename);

	luaL_openlibs(L); /* opens the standard libraries */

	if (luaL_loadfile(L, filename)) {
		ERROR("ERROR loading %s", filename);
		lua_close(L);
		return -1;
	}

	ret = lua_pcall(L, 0, 0, 0);
	if (ret) {
		LUAstackDump(L);
		ERROR("ERROR preparing Lua script %s %d",
			filename, ret);
		lua_close(L);
		return -1;
	}

	lua_getglobal(L, fnname);

	if(!lua_isfunction(L,lua_gettop(L))) {
		lua_close(L);
		TRACE("Script : no %s in %s script, exiting", fnname, filename);
		return 0;
	}

	/* passing arguments */
	lua_pushstring(L, filename);

	if (lua_pcall(L, 1, 2, 0)) {
		LUAstackDump(L);
		ERROR("ERROR Calling Lua script %s", filename);
		lua_close(L);
		return -1;
	}

	if (lua_type(L, 1) == LUA_TBOOLEAN)
		ret = lua_toboolean(L, 1) ? 0 : 1;

	if (lua_type(L, 2) == LUA_TSTRING) {
		output = lua_tostring(L, 2);
		TRACE("Script output: %s script end", output);
	}

	lua_close(L);

	return ret;

}

 __attribute__((constructor))
static void lua_handler(void)
{
	register_handler("lua", start_lua_script, NULL);
}
