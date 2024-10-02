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
	ROOT_DEV_PARTLABEL,
	ROOT_DEV_UNKNOWN
} root_dev_type;

void LUAstackDump (lua_State *L);
int run_lua_script(lua_State *L, const char *script, bool load, const char *function, char *parms);
lua_State *lua_session_init(struct dict *bootenv);
int lua_init(void);
int lua_load_buffer(lua_State *L, const char *buf);
int lua_parser_fn(lua_State *L, const char *fcn, struct img_type *img);
int lua_handler_fn(lua_State *L, const char *fcn, const char *parms);

int lua_notify_trace(lua_State *L);
int lua_notify_error(lua_State *L);
int lua_notify_info(lua_State *L);
int lua_notify_warn(lua_State *L);
int lua_notify_debug(lua_State *L);
int lua_notify_progress(lua_State *L);

int lua_get_swupdate_version(lua_State *L);

#define lua_exit(L) lua_close((lua_State *)L)

#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM  < 503
static inline int lua_isinteger (lua_State *L, int index) {
  if (lua_type(L, index) == LUA_TNUMBER) {
    lua_Number n = lua_tonumber(L, index);
    lua_Integer i = lua_tointeger(L, index);
    if (i == n)
      return 1;
  }
  return 0;
}
#endif

#else

struct img_type;

#define lua_State void
#define lua_exit(L)
#define lua_close(L)
static inline lua_State *lua_session_init(struct dict __attribute__ ((__unused__)) *bootenv) { return NULL;}
static inline int lua_init(void) { return 0; }
static inline int lua_load_buffer(lua_State __attribute__ ((__unused__)) *L, 
					const char __attribute__ ((__unused__)) *buf) {return 1;}
static inline int lua_parser_fn(lua_State __attribute__ ((__unused__)) *L,
			 const char __attribute__ ((__unused__)) *fcn,
			 struct img_type __attribute__ ((__unused__)) *img) { return -1; }
static inline int lua_handler_fn(lua_State __attribute__ ((__unused__)) *L,
			 const char __attribute__ ((__unused__)) *fcn,
			 const char __attribute__ ((__unused__)) *parms) { return -1; }
static inline int lua_handlers_init(lua_State __attribute__ ((__unused__)) *L) { return 0; }
#endif
