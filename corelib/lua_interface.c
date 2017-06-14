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

#include <stdlib.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lua_util.h"
#include "util.h"
#include "handler.h"

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

#ifdef CONFIG_HANDLER_IN_LUA
#include "lua_util.h"
static lua_State *gL = NULL;

/**
 * @brief wrapper to call the lua function
 *
 * The reference to the lua function is stored in the registry table.
 * To access the reference the index into this table is required. The
 * index is stored in the void* data pointer. This is due to the fact
 * that c can not store a direct reference to a lua object.
 *
 * @param sw [in] software struct which contains all installable images
 * @param index [in] defines which image have to be installed
 * @param unused [in] unused in this context
 * @param data [in] pointer to the index in the lua registry for the function
 * @return This function returns 0 if successfull and -1 if unsuccessfull.
 */
static int l_handler_wrapper(struct img_type *img, void *data) {
	int res = 0;
	lua_Number result;
	int l_func_ref;
	int fdout;

	if (!gL || !img || !data) {
		return -1;
	}

	/*
	 * if the image was not extracted, it loads
	 * images from a storage. Extract the file
	 * and copy it into TMPDIR
	 */
	if (!strlen(img->extract_file)) {
		snprintf(img->extract_file, sizeof(img->extract_file), "%s%s",
			TMPDIR, img->fname);
		fdout = openfileoutput(img->extract_file);
		res = extract_next_file(img->fdin, fdout, img->offset, 0,
					 img->is_encrypted, img->sha256);
	}

	l_func_ref = *((int*)data);
	/* get the callback function */
	lua_rawgeti(gL, LUA_REGISTRYINDEX, l_func_ref );
	image2table(gL, img);

	if (LUA_OK != (res = lua_pcall(gL, 1, 1, 0))) {
		ERROR("error while executing the lua callback: %d\n",res);
		puts(lua_tostring(gL, -1));
		return -1;
	}

	 /* retrieve result */
	if (!lua_isnumber(gL, -1)) {
		printf(" Lua Handler must return a number");
		return -1;
	}

	result = lua_tonumber(gL, -1);
	TRACE("[lua handler] returned: %d\n",(int)result);

	return (int) result;
}

/**
 * @brief function to register a callback from lua
 *
 * This function is exported to the lua stack and can be called
 * from any lua script in the same context (Stack)
 *
 * @param [in] the lua Stack
 * @return This function returns 0 if successfull and -1 if unsuccessfull.
 */
static int l_register_handler( lua_State *L ) {
	int *l_func_ref = malloc(sizeof(int));
	if(!l_func_ref) {
		ERROR("lua handler: unable to allocate memory\n");
		return -1;
	} else {
		const char *handler_desc = luaL_checkstring(L, 1);
		/* store the callback function in registry */
		*l_func_ref = luaL_ref (L, LUA_REGISTRYINDEX);
		/* pop the arguments from the stack */
		lua_pop (L, 2);
		register_handler(handler_desc,l_handler_wrapper,l_func_ref);
		return 0;
	}
}

/**
 * @brief function to send notifications to the recovery from lua
 *
 * This function is exported to the lua stack and can be called
 * from any lua script in the same context (Stack)
 *
 * @param [in] the lua Stack
 * @return This function returns 0 if successfull and -1 if unsuccessfull.
 */
static int l_notify (lua_State *L) {
	lua_Number status =  luaL_checknumber (L, 1);
	lua_Number error  =  luaL_checknumber (L, 2);
	const char *msg   =  luaL_checkstring (L, 3);

	if (strlen(msg))
		notify((RECOVERY_STATUS)status, (int)error, msg);

	return 0;
}

/**
 * @brief array with the function which are exported to lua
 */
static const luaL_Reg l_swupdate[] = {
        { "register_handler", l_register_handler },
        { "notify", l_notify },
        { NULL, NULL }
};

static void lua_push_enum(lua_State *L, const char *name, int value)
{
	lua_pushstring(L, name);
	lua_pushnumber(L, (lua_Number) value );
	lua_settable(L, -3);
}

/**
 * @brief function to register the swupdate package in the lua Stack
 *
 * @param [in] the lua Stack
 * @return This function returns 0 if successfull and -1 if unsuccessfull.
 */
static int luaopen_swupdate(lua_State *L) {
	luaL_newlib (L, l_swupdate);

	/* export the recovery status enum */
	lua_pushstring(L, "RECOVERY_STATUS");
	lua_newtable (L);
	lua_push_enum(L, "IDLE", IDLE);
	lua_push_enum(L, "START", START);
	lua_push_enum(L, "RUN", RUN);
	lua_push_enum(L, "SUCCESS", SUCCESS);
	lua_push_enum(L, "FAILURE", FAILURE);
	lua_push_enum(L, "DOWNLOAD", DOWNLOAD);
	lua_push_enum(L, "DONE", DONE);
	lua_settable(L, -3);

	return 1;
}

int lua_handlers_init(void)
{
	int ret = -1;

	gL = NULL;
	gL = luaL_newstate();
	if (gL) {
		printf("Searching for custom LUA handlers :");
		/* load standard libraries */
		luaL_openlibs(gL);
		luaL_requiref( gL, "swupdate", luaopen_swupdate, 1 );
		/* try to load lua handlers for the swupdate system */
		ret = luaL_dostring(gL,"require (\"swupdate_handlers\")");
		if(ret != 0)
		{
			TRACE("No lua handler found:\n%s", lua_tostring(gL, -1));
		} else
			printf(" OK\n");
	} else	{
		printf ("Unable to register Lua context for callbacks\n");
	}

	return ret;
}
#else
int lua_handlers_init(void) {return 0;}
#endif
