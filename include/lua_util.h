/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#ifndef _LUA_UTIL_H
#define _LUA_UTIL_H

#ifdef CONFIG_LUA
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "util.h"

void LUAstackDump (lua_State *L);
int run_lua_script(const char *script, const char *function, char *parms);
lua_State *lua_parser_init(const char *buf);
int lua_parser_fn(lua_State *L, const char *fcn, struct img_type *img);
int lua_handlers_init(void);
#define lua_parser_exit(L) lua_close((lua_State *)L)

#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM == 501
#define LUA_OK 0
#define luaL_newlib(L, l) (lua_newtable((L)),luaL_setfuncs((L), (l), 0))
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
#endif

#else

#define lua_State void
#define lua_parser_exit(L)
static inline lua_State *lua_parser_init(const char __attribute__ ((__unused__)) *buf) { return NULL;}
static inline int lua_parser_fn(lua_State __attribute__ ((__unused__)) *L,
			 const char __attribute__ ((__unused__)) *fcn,
			 struct img_type __attribute__ ((__unused__)) *img) { return -1; }
static inline int lua_handlers_init(void) { return 0; }
#endif


#endif
