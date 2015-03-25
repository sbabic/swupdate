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

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lua_util.h"
#include "util.h"

void LUAstackDump (lua_State *L) {
	int i;
	int top = lua_gettop(L);
	for (i = 1; i <= top; i++) { /* repeat for each level */
		int t = lua_type(L, i);
		switch (t) {
			case LUA_TSTRING: { /* strings */
						  printf("’%s’", lua_tostring(L, i));
						  break;
					  }
			case LUA_TBOOLEAN: { /* booleans */
						   printf(lua_toboolean(L, i) ? "true" : "false");
						   break;
					   }
			case LUA_TNUMBER: { /* numbers */
						  printf("%g", lua_tonumber(L, i));
						  break;
					  }
			default: { /* other values */
					 printf("%s", lua_typename(L, t));
					 break;
				 }
		}
		printf(" "); /* put a separator */
	}
	printf("\n"); /* end the listing */
}

int run_lua_script(char *script, char *function, char *parms)
{
	int ret;

	lua_State *L = luaL_newstate(); /* opens Lua */
	luaL_openlibs(L); /* opens the standard libraries */

	if (luaL_loadfile(L, script)) {
		ERROR("ERROR loading %s", script);
		return 1;
	}

	ret = lua_pcall(L, 0, 0, 0);
	if (ret) {
		LUAstackDump(L);
		ERROR("ERROR preparing %s script %d", script, ret);
		return 1;
	}

	lua_getglobal(L, function);
	/* passing arguments */
	lua_pushstring(L, parms);

	if (lua_pcall(L, 1, 1, 0)) {
		LUAstackDump(L);
		ERROR("ERROR running script");
		return 1;
	}

	if (lua_type(L, 1) != LUA_TNUMBER) {
		ERROR("LUA script returns wrong type");
		lua_close(L);
		return 1;
	}

	ret = lua_tonumber(L, 1);
	lua_close(L);

	return ret;

}

/**
 * @brief convert a image description struct to a lua table
 *
 * @param L [inout] the lua stack
 * @param software [in] the software struct
 */
void image2table(lua_State* L, struct img_type *img) {
	if (L && img) {
		lua_newtable (L);
		lua_pushstring(L, "filename");
		lua_pushstring(L, img->fname);
		lua_settable(L, -3);
		lua_pushstring(L, "volname");
		lua_pushstring(L, img->volname);
		lua_settable(L, -3);
		lua_pushstring(L, "type");
		lua_pushstring(L, img->type);
		lua_settable(L, -3);

		lua_pushstring(L, "device");
		lua_pushstring(L, img->device);
		lua_settable(L, -3);
		lua_pushstring(L, "path");
		lua_pushstring(L, img->path);
		lua_settable(L, -3);
		lua_pushstring(L, "extracted");
		lua_pushstring(L, img->extract_file);
		lua_settable(L, -3);

		lua_pushstring(L, "required");
		lua_pushboolean(L, img->required);
		lua_settable(L, -3);

		lua_pushstring(L, "compressed");
		lua_pushboolean(L, img->compressed);
		lua_settable(L, -3);
		lua_pushstring(L, "required");
		lua_pushboolean(L, img->required);
		lua_settable(L, -3);
		lua_pushstring(L, "provided");
		lua_pushboolean(L, img->provided);
		lua_settable(L, -3);
		lua_pushstring(L, "is_script");
		lua_pushboolean(L, img->is_script);
		lua_settable(L, -3);
		lua_pushstring(L, "offset");
		lua_pushnumber(L, (double)img->offset);
		lua_settable(L, -3);
		lua_pushstring(L, "size");
		lua_pushnumber(L, (double)img->size);
		lua_settable(L, -3);
		lua_pushstring(L, "checksum");
		lua_pushnumber(L, (double)img->checksum);
		lua_settable(L, -3);
	}
}
