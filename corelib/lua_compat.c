/*
 * Author: Christian Storm
 * Copyright (C) 2017, Siemens AG
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#include <lua.h>
#include <lua_util.h>

/*
 * These LuaJIT/Lua 5.1 compatibility functions are taken from
 * https://github.com/keplerproject/lua-compat-5.2
 */
#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM == 501
void luaL_setfuncs(lua_State *L, const luaL_Reg *l, int nup)
{
	luaL_checkstack(L, nup+1, "too many upvalues");
	for (; l->name != NULL; l++) {  /* fill the table with given functions */
		int i;
		lua_pushstring(L, l->name);
		for (i = 0; i < nup; i++)  /* copy upvalues to the top */
		lua_pushvalue(L, -(nup + 1));
		lua_pushcclosure(L, l->func, nup);  /* closure with those upvalues */
		lua_settable(L, -(nup + 3)); /* table must be below the upvalues, the name and the closure */
	}
	lua_pop(L, nup);  /* remove upvalues */
}

void luaL_requiref(lua_State *L, char const* modname, lua_CFunction openf, int glb)
{
	luaL_checkstack(L, 3, "not enough stack slots");
	lua_pushcfunction(L, openf);
	lua_pushstring(L, modname);
	lua_call(L, 1, 1);
	lua_getglobal(L, "package");
	lua_getfield(L, -1, "loaded");
	lua_replace(L, -2);
	lua_pushvalue(L, -2);
	lua_setfield(L, -2, modname);
	lua_pop(L, 1);
	if (glb) {
		lua_pushvalue(L, -1);
		lua_setglobal(L, modname);
	}
}
#endif
