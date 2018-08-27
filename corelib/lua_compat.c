/*
 * Author: Christian Storm
 * Copyright (C) 2017, Siemens AG
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#include <lua.h>
#include <lua_util.h>

/*
 * These LuaJIT/Lua 5.1 compatibility functions are taken from
 * https://github.com/keplerproject/lua-compat-5.2
 */
#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM == 501
char *luaL_prepbuffsize(luaL_Buffer_52 *B, size_t s)
{
	if (B->capacity - B->nelems < s) { /* needs to grow */
		char* newptr = NULL;
		size_t newcap = B->capacity * 2;
		if (newcap - B->nelems < s)
			newcap = B->nelems + s;
		if (newcap < B->capacity) /* overflow */
			luaL_error(B->L2, "buffer too large");
		newptr = lua_newuserdata(B->L2, newcap);
		memcpy(newptr, B->ptr, B->nelems);
		if (B->ptr != B->b.buffer)
			lua_replace(B->L2, -2); /* remove old buffer */
		B->ptr = newptr;
		B->capacity = newcap;
	}
	return B->ptr+B->nelems;
}

void luaL_buffinit(lua_State *L, luaL_Buffer_52 *B)
{
	/* make it crash if used via pointer to a 5.1-style luaL_Buffer */
	B->b.p = NULL;
	B->b.L = NULL;
	B->b.lvl = 0;
	/* reuse the buffer from the 5.1-style luaL_Buffer though! */
	B->ptr = B->b.buffer;
	B->capacity = LUAL_BUFFERSIZE;
	B->nelems = 0;
	B->L2 = L;
}

void luaL_pushresult(luaL_Buffer_52 *B)
{
	lua_pushlstring(B->L2, B->ptr, B->nelems);
	if (B->ptr != B->b.buffer)
		lua_replace(B->L2, -2); /* remove userdata buffer */
}

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
