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
#include <stdbool.h>
#include <errno.h>
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

#ifdef CONFIG_HANDLER_IN_LUA
static int l_register_handler( lua_State *L );
#endif

void LUAstackDump(lua_State *L)
{
	int top = lua_gettop(L);
	for (int i = 1; i <= top; i++) {
		int t = lua_type(L, i);
		switch (t) {
			case LUA_TSTRING: {
				TRACE("(%d) [string] %s", i, lua_tostring(L, i));
				break;
			}
			case LUA_TBOOLEAN: {
				TRACE("(%d) [bool  ] %s", i, (lua_toboolean(L, i) ? "true" : "false"));
				break;
			}
			case LUA_TFUNCTION: {
				TRACE("(%d) [func  ] %s()", i, lua_tostring(L, i));
				break;
			}
			case LUA_TNUMBER: {
				TRACE("(%d) [number] %g", i, lua_tonumber(L, i));
				break;
			}
			case LUA_TTABLE: {
				lua_pushvalue(L, -1);
				lua_pushnil(L);
				/* Stack: nil, table */
				while (lua_next(L, -2)) {
					/* Stack: value, key, table */
					lua_pushvalue(L, -2);
					/* Stack: key, value, key, table */
					switch(lua_type(L, -2)) {
						case LUA_TSTRING:
						case LUA_TNUMBER:
							TRACE("(%d) [table ] %s = %s", i,
								lua_tostring(L, -1),
								lua_tostring(L, -2));
							break;
						case LUA_TFUNCTION:
							TRACE("(%d) [table ] %s()", i,
								lua_tostring(L, -1));
							break;
						case LUA_TTABLE:
							TRACE("(%d) [table ] %s <table>", i,
								lua_tostring(L, -1));
							break;
						case LUA_TBOOLEAN:
							TRACE("(%d) [table ] %s = %s", i,
								lua_tostring(L, -1),
								(lua_toboolean(L, -2) ? "true" : "false"));
							break;
						default:
							TRACE("(%d) [table ] %s = <unparsed type>", i,
								lua_tostring(L, -1));
					}
					lua_pop(L, 2);
					/* Stack: key, table */
				}
				/* Stack: table */
				lua_pop(L, 1);
				/* Stack: <empty> */
				break;
			}
			default: {
				TRACE("(%d) [      ] unparsed type %s", i, lua_typename(L, t));
				break;
			}
		}
	}
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
		ERROR("Lua script returns wrong type");
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

static void lua_string_to_img(struct img_type *img, const char *key,
	       const char *value)
{
	const char offset[] = "offset";
	char seek_str[MAX_SEEK_STRING_SIZE];
	char *endp = NULL;

	if (!strcmp(key, "name")) {
		strncpy(img->id.name, value,
			sizeof(img->id.name));
	}
	if (!strcmp(key, "version")) {
		strncpy(img->id.version, value,
			sizeof(img->id.version));
	}
	if (!strcmp(key, "filename")) {
		strncpy(img->fname, value,
			sizeof(img->fname));
	}
	if (!strcmp(key, "volume"))
		strncpy(img->volname, value,
			sizeof(img->volname));
	if (!strcmp(key, "type"))
		strncpy(img->type, value,
			sizeof(img->type));
	if (!strcmp(key, "device"))
		strncpy(img->device, value,
			sizeof(img->device));
	if (!strcmp(key, "mtdname"))
		strncpy(img->path, value,
			sizeof(img->path));
	if (!strcmp(key, "path"))
		strncpy(img->path, value,
			sizeof(img->path));
	if (!strcmp(key, "data"))
		strncpy(img->type_data, value,
			sizeof(img->type_data));
	if (!strcmp(key, "filesystem"))
		strncpy(img->filesystem, value,
			sizeof(img->filesystem));
	if (!strcmp(key, "sha256"))
		ascii_to_hash(img->sha256, value);

	if (!strncmp(key, offset, sizeof(offset))) {
		strncpy(seek_str, value,
			sizeof(seek_str));
		/* convert the offset handling multiplicative suffixes */
		if (seek_str != NULL && strnlen(seek_str, MAX_SEEK_STRING_SIZE) != 0) {
			errno = 0;
			img->seek = ustrtoull(seek_str, &endp, 0);
			if (seek_str == endp || (img->seek == ULLONG_MAX && \
					errno == ERANGE)) {
				ERROR("offset argument: ustrtoull failed");
				return;
			}
		} else
			img->seek = 0;
	}
}


static void lua_bool_to_img(struct img_type *img, const char *key,
	       bool val)
{
	if (!strcmp(key, "compressed"))
		img->compressed = (bool)val;
	if (!strcmp(key, "installed-directly"))
		img->install_directly = (bool)val;
	if (!strcmp(key, "install_if_different"))
		img->id.install_if_different = (bool)val;
	if (!strcmp(key, "encrypted"))
		img->is_encrypted = (bool)val;
}

static void lua_number_to_img(struct img_type *img, const char *key,
	       double val)
{
	if (!strcmp(key, "offset"))
		img->seek = (unsigned long long)val;

}

static void image2table(lua_State* L, struct img_type *img) {
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
		LUA_PUSH_IMG_STRING(img, "filesystem", filesystem);

		LUA_PUSH_IMG_BOOL(img, "compressed", compressed);
		LUA_PUSH_IMG_BOOL(img, "installed_directly", install_directly);
		LUA_PUSH_IMG_BOOL(img, "install_if_different", id.install_if_different);
		LUA_PUSH_IMG_BOOL(img, "encrypted", is_encrypted);
		LUA_PUSH_IMG_BOOL(img, "partition", is_partitioner);
		LUA_PUSH_IMG_BOOL(img, "script", is_script);

		LUA_PUSH_IMG_NUMBER(img, "offset", seek);
		LUA_PUSH_IMG_NUMBER(img, "size", size);
		LUA_PUSH_IMG_NUMBER(img, "checksum", checksum);

		char *hashstring = alloca(2 * SHA256_HASH_LENGTH + 1);
		hash_to_ascii(img->sha256, hashstring);
		lua_pushstring(L, "sha256");
		lua_pushstring(L, hashstring);
		lua_settable(L, -3);
	}
}

static void table2image(lua_State* L, struct img_type *img) {
	if (L && img && (lua_type(L, -1) == LUA_TTABLE)) {
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			int t = lua_type(L, -1);
			switch (t) {
				case LUA_TSTRING: /* strings */
					lua_string_to_img(img, lua_tostring(L, -2), lua_tostring(L, -1));
					break;
				case LUA_TBOOLEAN: /* booleans */
					lua_bool_to_img(img, lua_tostring(L, -2), lua_toboolean(L, -1));
					break;
				case LUA_TNUMBER: /* numbers */
					lua_number_to_img(img, lua_tostring(L, -2), lua_tonumber(L, -1));
					break;
			}
			lua_pop(L, 1);
		}
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
		notify((RECOVERY_STATUS)status, (int)error, INFOLEVEL, msg);

	lua_pop(L, 3);
	return 0;
}

static int l_trace(lua_State *L) {
	const char *msg = luaL_checkstring (L, 1);

	if (strlen(msg))
		TRACE("%s", msg);

	lua_pop(L, 1);
	return 0;
}

static int l_error(lua_State *L) {
	const char *msg = luaL_checkstring (L, 1);

	if (strlen(msg))
		ERROR("%s", msg);

	lua_pop(L, 1);
	return 0;
}

static int l_info(lua_State *L) {
	const char *msg = luaL_checkstring (L, 1);

	if (strlen(msg))
		INFO("%s", msg);

	lua_pop(L, 1);
	return 0;
}

/**
 * @brief array with the function which are exported to lua
 */
static const luaL_Reg l_swupdate[] = {
#ifdef CONFIG_HANDLER_IN_LUA
        { "register_handler", l_register_handler },
#endif
        { "notify", l_notify },
        { "error", l_error },
        { "trace", l_trace },
        { "info", l_info },
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
 * @return 1 (nr. of results on stack, the 'swupdate' module table)
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
	lua_push_enum(L, "SUBPROCESS", SUBPROCESS);
	lua_settable(L, -3);

#ifdef CONFIG_HANDLER_IN_LUA
	/* export the handler mask enum */
	lua_pushstring(L, "HANDLER_MASK");
	lua_newtable (L);
	lua_push_enum(L, "IMAGE_HANDLER", IMAGE_HANDLER);
	lua_push_enum(L, "FILE_HANDLER", FILE_HANDLER);
	lua_push_enum(L, "SCRIPT_HANDLER", SCRIPT_HANDLER);
	lua_push_enum(L, "BOOTLOADER_HANDLER", BOOTLOADER_HANDLER);
	lua_push_enum(L, "PARTITION_HANDLER", PARTITION_HANDLER);
	lua_push_enum(L, "ANY_HANDLER", ANY_HANDLER);
	lua_settable(L, -3);
#endif

	return 1;
}

#ifdef CONFIG_HANDLER_IN_LUA
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
	const char* TMPDIR = get_tmpdir();

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
 * @return This function returns 0 values back to Lua.
 */
static int l_register_handler( lua_State *L ) {
	int *l_func_ref = malloc(sizeof(int));
	if(!l_func_ref) {
		ERROR("lua handler: unable to allocate memory\n");
		lua_pop(L, 2);
		return 0;
	} else {
		unsigned int mask = ANY_HANDLER;
		if (lua_isnumber(L, 3)) {
			mask = luaL_checknumber(L, 3);
			lua_pop(L, 1);
		}
		const char *handler_desc = luaL_checkstring(L, 1);
		/* store the callback function in registry */
		*l_func_ref = luaL_ref (L, LUA_REGISTRYINDEX);
		/* cleanup stack */
		lua_pop (L, 1);

		register_handler(handler_desc, l_handler_wrapper,
				 mask, l_func_ref);
		return 0;
	}
}

int lua_handlers_init(void)
{
	int ret = -1;

	gL = luaL_newstate();
	if (gL) {
		/* load standard libraries */
		luaL_openlibs(gL);
		luaL_requiref( gL, "swupdate", luaopen_swupdate, 1 );
		lua_pop(gL, 1); /* remove unused copy left on stack */
		/* try to load lua handlers for the swupdate system */
		if ((ret = luaL_dostring(gL, "require (\"swupdate_handlers\")")) != 0) {
			INFO("No Lua handler(s) found.");
			if (luaL_dostring(gL, "return package.path:gsub(';','\\n'):gsub('?','swupdate_handlers')") == 0) {
				lua_pop(gL, 1);
				TRACE("Lua handler search path:\n%s", lua_tostring(gL, -1));
				lua_pop(gL, 1);
			}
		} else {
			INFO("Lua handler(s) found.");
		}
	} else	{
		WARN("Unable to register Lua context for callbacks\n");
	}

	return ret;
}
#else
int lua_handlers_init(void) {return 0;}
#endif

lua_State *lua_parser_init(const char *buf)
{
	lua_State *L = luaL_newstate(); /* opens Lua */

	if (!L)
		return NULL;
	luaL_openlibs(L); /* opens the standard libraries */
	luaL_requiref(L, "swupdate", luaopen_swupdate, 1 );
	lua_pop(L, 1); /* remove unused copy left on stack */
	if (luaL_loadstring(L, buf) || lua_pcall(L, 0, 0, 0)) {
		LUAstackDump(L);
		ERROR("ERROR preparing Lua embedded script in parser");
		lua_close(L);
		return NULL;
	}

	return L;
}

int lua_parser_fn(lua_State *L, const char *fcn, struct img_type *img)
{
	int ret = -1;

	lua_getglobal(L, fcn);
	if(!lua_isfunction(L, lua_gettop(L))) {
		lua_pop(L, 1);
		TRACE("Script : no %s in script, exiting", fcn);
		return -1;
	}
	TRACE("Prepared to run %s", fcn);

	/*
	 * passing arguments
	 */
	image2table(L, img);

	if (lua_pcall(L, 1, 2, 0)) {
		LUAstackDump(L);
		ERROR("ERROR Calling Lua %s", fcn);
		return -1;
	}

	if (lua_type(L, -2) == LUA_TBOOLEAN)
		ret = lua_toboolean(L, -2) ? 0 : 1;

	LUAstackDump(L);

	table2image(L, img);

	lua_pop(L, 2); /* clear stack */

	TRACE("Script returns %d", ret);

	return ret;
}
