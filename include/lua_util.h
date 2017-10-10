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


#ifndef _LUA_UTIL_H
#define _LUA_UTIL_H

#ifdef CONFIG_LUA
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "util.h"

void LUAstackDump (lua_State *L);
int run_lua_script(char *script, char *function, char *parms);
lua_State *lua_parser_init(const char *buf);
int lua_parser_fn(lua_State *L, const char *fcn, struct img_type *img);
int lua_handlers_init(void);
#define lua_parser_exit(L) lua_close((lua_State *)L)

#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM == 501
#define LUA_OK 0
#define luaL_newlib(L, l) (lua_newtable((L)),luaL_setfuncs((L), (l), 0))
void luaL_setfuncs(lua_State *L, const luaL_Reg *l, int nup);
void luaL_requiref(lua_State *L, char const* modname, lua_CFunction openf, int glb);
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
