/*
 * (C) Copyright 2024 Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#pragma once
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <lua.h>
#include "lauxlib.h"
#include "lualib.h"

#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM == 501
#define LUA_OK 0
#if !defined(luaL_newlib)
#define luaL_newlib(L, l) (lua_newtable((L)),luaL_setfuncs((L), (l), 0))
#endif

void luaL_setfuncs(lua_State *L, const luaL_Reg *l, int nup);
void luaL_requiref(lua_State *L, char const* modname, lua_CFunction openf, int glb);


/*
 * See https://github.com/keplerproject/lua-compat-5.3/wiki/luaL_Stream
 * on the reason for the absence of luaL_Stream's closef member and
 * compatibility with LuaJIT / Lua 5.1.
 */
typedef struct luaL_Stream {
  FILE *f;
} luaL_Stream;

typedef struct luaL_Buffer_52 {
	luaL_Buffer b; /* make incorrect code crash! */
	char *ptr;
	size_t nelems;
	size_t capacity;
	lua_State *L2;
} luaL_Buffer_52;
#define luaL_Buffer luaL_Buffer_52

#define luaL_prepbuffsize luaL_prepbuffsize_52
char *luaL_prepbuffsize(luaL_Buffer_52 *B, size_t s);

#define luaL_buffinit luaL_buffinit_52
void luaL_buffinit(lua_State *L, luaL_Buffer_52 *B);

#undef luaL_addsize
#define luaL_addsize(B, s) ((B)->nelems += (s))

#define luaL_pushresult luaL_pushresult_52
void luaL_pushresult(luaL_Buffer_52 *B);

#define luaL_checkversion(L) ((void)0)
#define lua_rawlen(L, i) lua_objlen(L, i)

#endif

