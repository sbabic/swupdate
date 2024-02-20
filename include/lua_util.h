/*
 * (C) Copyright 2013-2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once
#ifdef CONFIG_LUA
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "util.h"

typedef enum {
	ROOT_DEV_PATH,
	ROOT_DEV_UUID,
	ROOT_DEV_PARTUUID,
	ROOT_DEV_PARTLABEL
} root_dev_type;

void LUAstackDump (lua_State *L);
int run_lua_script(lua_State *L, const char *script, bool load, const char *function, char *parms);
lua_State *lua_init(struct dict *bootenv);
int lua_load_buffer(lua_State *L, const char *buf);
int lua_parser_fn(lua_State *L, const char *fcn, struct img_type *img);
int lua_handlers_init(void);

int lua_notify_trace(lua_State *L);
int lua_notify_error(lua_State *L);
int lua_notify_info(lua_State *L);
int lua_notify_warn(lua_State *L);
int lua_notify_debug(lua_State *L);
int lua_notify_progress(lua_State *L);

int lua_get_swupdate_version(lua_State *L);

#define lua_exit(L) lua_close((lua_State *)L)

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

#else

struct img_type;

#define lua_State void
#define lua_exit(L)
#define lua_close(L)
static inline lua_State *lua_init(struct dict __attribute__ ((__unused__)) *bootenv) { return NULL;}
static inline int lua_load_buffer(lua_State __attribute__ ((__unused__)) *L, 
					const char __attribute__ ((__unused__)) *buf) {return 1;}
static inline int lua_parser_fn(lua_State __attribute__ ((__unused__)) *L,
			 const char __attribute__ ((__unused__)) *fcn,
			 struct img_type __attribute__ ((__unused__)) *img) { return -1; }
static inline int lua_handlers_init(void) { return 0; }
#endif
