/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * See file CREDITS for list of people who contributed to this
 * project.
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include "swupdate.h"
#include "handler.h"
#include "lua_util.h"

#define MAX_INSTALLER_HANDLER	64
struct installer_handler supported_types[MAX_INSTALLER_HANDLER];
static unsigned long nr_installers = 0;

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
		res = extract_next_file(img->fdin, fdout, img->offset, 0);
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

	return (int) res;
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
	lua_settable(L, -3);

	return 1;
}

void lua_handlers_init(void)
{
	gL = NULL;
	gL = luaL_newstate();
	if (gL) {
		/* load standard libraries */
		luaL_openlibs(gL);
		luaL_requiref( gL, "swupdate", luaopen_swupdate, 1 );
		/* try to load lua handlers for the swupdate system */
		if(luaL_dostring(gL,"require (\"swupdate_handlers\")") != 0)
		{
			puts(lua_tostring(gL, -1));
		}
	} else	{
		printf ("Unable to register Lua context for callbacks\n");
	}
}
#else
void lua_handlers_init(void) {}
#endif

int register_handler(const char *desc, 
		handler installer, void *data)
{

	if (nr_installers > MAX_INSTALLER_HANDLER - 1)
		return -1;

	strncpy(supported_types[nr_installers].desc, desc,
		      sizeof(supported_types[nr_installers].desc));
	supported_types[nr_installers].installer = installer;
	supported_types[nr_installers].data = data;
	nr_installers++;

	return 0;
}

void print_registered_handlers(void)
{
	unsigned int i;

	if (!nr_installers)
		return;

	printf("Registered handlers:\n");
	for (i = 0; i < nr_installers; i++) {
		printf("\t%s\n", supported_types[i].desc);
	}
}

struct installer_handler *find_handler(struct img_type *img)
{
	unsigned int i;

	for (i = 0; i < nr_installers; i++) {
		if ((strlen(img->type) == strlen(supported_types[i].desc)) &&
			strcmp(img->type,
				supported_types[i].desc) == 0)
			break;
	}
	if (i >= nr_installers)
		return NULL;
	return &supported_types[i];
}
