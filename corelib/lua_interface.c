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

#define LUA_PUSH_IMG_STRING(img, attr, field)  do { \
	lua_pushstring(L, attr);		\
	lua_pushstring(L, img->field);		\
	lua_settable(L, -3);			\
} while (0)

#define LUA_PUSH_IMG_BOOL(img, attr, field)  do { \
	lua_pushstring(L, attr);		\
	lua_pushboolean(L, img->field);		\
	lua_settable(L, -3);			\
} while (0)

#define LUA_PUSH_IMG_NUMBER(img, attr, field)  do { \
	lua_pushstring(L, attr);		\
	lua_pushnumber(L, (double)img->field);	\
	lua_settable(L, -3);			\
} while (0)

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
		LUA_PUSH_IMG_STRING(img, "name", id.name);
		LUA_PUSH_IMG_STRING(img, "version", id.version);
		LUA_PUSH_IMG_STRING(img, "filename", fname);
		LUA_PUSH_IMG_STRING(img, "volume", volname);
		LUA_PUSH_IMG_STRING(img, "type", type);
		LUA_PUSH_IMG_STRING(img, "device", device);
		LUA_PUSH_IMG_STRING(img, "path", path);
		LUA_PUSH_IMG_STRING(img, "mtdname", path);
		LUA_PUSH_IMG_STRING(img, "data", type_data);

		LUA_PUSH_IMG_BOOL(img, "compressed", compressed);
		LUA_PUSH_IMG_BOOL(img, "installed-directly", compressed);
		LUA_PUSH_IMG_BOOL(img, "install-if-different", compressed);
		LUA_PUSH_IMG_BOOL(img, "encrypted", is_encrypted);
		LUA_PUSH_IMG_BOOL(img, "partition", is_partitioner);
		LUA_PUSH_IMG_BOOL(img, "script", is_script);

		LUA_PUSH_IMG_NUMBER(img, "offset", seek);
		LUA_PUSH_IMG_NUMBER(img, "size", size);
		LUA_PUSH_IMG_NUMBER(img, "checksum", checksum);

	}
}
