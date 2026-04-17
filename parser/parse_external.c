/*
 * (C) Copyright 2013-2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */


#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "generated/autoconf.h"
#include "swupdate.h"
#include "parsers.h"
#include "hw-compatibility.h"

#ifdef CONFIG_LUAEXTERNAL
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "util.h"
#include "lua_util.h"
#ifndef CONFIG_SETEXTPARSERNAME
#define LUA_PARSER	"lua-tools/extparser.lua"
#else
#define LUA_PARSER	(CONFIG_EXTPARSERNAME)
#endif

typedef void (*stream_handler_fn)(struct img_type *img, const char *value);

struct stream_handler_entry {
	const char *key;
	stream_handler_fn handler;
};

DEFINE_IMG_STRLCPY_SETTER(sw_set_type, type)
DEFINE_IMG_STRLCPY_SETTER(sw_set_name, id.name)
DEFINE_IMG_STRLCPY_SETTER(sw_set_version, id.version)
DEFINE_IMG_STRLCPY_SETTER(sw_set_mtdname, mtdname)
DEFINE_IMG_STRLCPY_SETTER(sw_set_filesystem, filesystem)
DEFINE_IMG_STRLCPY_SETTER(sw_set_volume, volname)
DEFINE_IMG_STRLCPY_SETTER(sw_set_device, device)
DEFINE_IMG_STRLCPY_SETTER(sw_set_path, path)

static void sw_set_filename(struct img_type *img, const char *value)
{
	strlcpy(img->fname, value, sizeof(img->fname));
	img->skip = SKIP_NONE;
}

static void sw_set_offset(struct img_type *img, const char *value)
{
	char seek_str[MAX_SEEK_STRING_SIZE];

	strlcpy(seek_str, value, sizeof(seek_str));
	/* convert the offset handling multiplicative suffixes */
	img->seek = ustrtoull(seek_str, NULL, 0);
	if (errno) {
		ERROR("offset argument: ustrtoull failed");
	}
}

static void sw_set_script(struct img_type *img, const char *value)
{
	(void)value;
	img->is_script = 1;
}

static void sw_set_sha256(struct img_type *img, const char *value)
{
	ascii_to_hash(img->sha256, value);
}

static void sw_set_encrypted(struct img_type *img, const char *value)
{
	(void)value;
	img->is_encrypted = true;
}

static void sw_set_compressed(struct img_type *img, const char *value)
{
	if (value == NULL || compressed_string_to_type(value, &img->compressed) < 0) {
		img->compressed = COMPRESSED_TRUE;
	}

}

static void sw_set_install_directly(struct img_type *img, const char *value)
{
	(void)value;
	img->install_directly = 1;
}

static void sw_set_install_if_different(struct img_type *img, const char *value)
{
	(void)value;
	img->id.install_if_different = 1;
}

static void sw_set_install_if_higher(struct img_type *img, const char *value)
{
	(void)value;
	img->id.install_if_higher = 1;
}

static const struct stream_handler_entry handlers[] = {
	{ "type", sw_set_type },
	{ "filename", sw_set_filename },
	{ "name", sw_set_name },
	{ "version", sw_set_version },
	{ "mtdname", sw_set_mtdname },
	{ "dest", sw_set_mtdname },
	{ "filesystem", sw_set_filesystem },
	{ "volume", sw_set_volume },
	{ "device_id", sw_set_device },
	{ "device", sw_set_device },
	{ "offset", sw_set_offset },
	{ "script", sw_set_script },
	{ "path", sw_set_path },
	{ "sha256", sw_set_sha256 },
	{ "encrypted", sw_set_encrypted },
	{ "compressed", sw_set_compressed },
	{ "installed-directly", sw_set_install_directly },
	{ "install-if-different", sw_set_install_if_different },
	{ "install-if-higher", sw_set_install_if_higher },
};

static void sw_append_stream(struct img_type *img, const char *key,
	       const char *value)
{
	size_t i;

	for (i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
		if (strcmp(key, handlers[i].key))
			continue;

		handlers[i].handler(img, value);
		return;
	}
}

int parse_external(struct swupdate_cfg *software, const char *filename,
		   char __attribute__((__unused__)) **error)
{
	int ret;
	unsigned int nstreams;
	struct img_type *image;
	struct hw_type hardware = {0};

	lua_State *L = luaL_newstate(); /* opens Lua */
	luaL_openlibs(L); /* opens the standard libraries */

	if (luaL_loadfile(L, LUA_PARSER)) {
		ERROR("ERROR loading %s", LUA_PARSER);
		lua_close(L);
		return 1;
	}

	ret = lua_pcall(L, 0, 0, 0);
	if (ret) {
		LUAstackDump(L);
		ERROR("ERROR preparing Parser in Lua %d", ret);

		return 1;
	}

	if (-1 == get_hw_revision(&hardware))
	{
	    ERROR("ERROR getting hw revision");
	    return 1;
	}

	lua_getglobal(L, "xmlparser");

	/* passing arguments */
	lua_pushstring(L, filename);
	lua_pushstring(L, hardware.boardname);
	lua_pushstring(L, hardware.revision);

	if (lua_pcall(L, 3, 4, 0)) {
		LUAstackDump(L);
		ERROR("ERROR Calling XML Parser in Lua");
		lua_close(L);
		return 1;
	}

	if (lua_type(L, 1) == LUA_TSTRING)
		strlcpy(software->name, lua_tostring(L, 1),
				sizeof(software->name));

	if (lua_type(L, 2) == LUA_TSTRING)
		strlcpy(software->version, lua_tostring(L, 2),
				sizeof(software->version));
	nstreams = 0;
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		printf("%s - %s\n",
		lua_typename(L, lua_type(L, -2)),
			lua_typename(L, lua_type(L, -1)));

		if (lua_type(L, -1) == LUA_TTABLE) {
			lua_pushnil(L);
			image = (struct img_type *)calloc(1, sizeof(struct img_type));
			if (!image) {
				ERROR( "No memory: malloc failed");
				return -ENOMEM;
			}
			while (lua_next(L, -2) != 0) {
				sw_append_stream(image, lua_tostring(L, -2),
					       lua_tostring(L, -1));

	       			lua_pop(L, 1);
			}
			if (image->is_script)
				LIST_INSERT_HEAD(&software->scripts, image, next);
			else
				LIST_INSERT_HEAD(&software->images, image, next);
			nstreams++;
		}

	       /* removes 'value'; keeps 'key' for next iteration */
	       lua_pop(L, 1);
	}

	LUAstackDump(L);

	lua_close(L);

	TRACE("Software: %s %s", software->name, software->version);
	LIST_FOREACH(image, &software->images, next) {
		TRACE("\tName: %s Type: %s", image->fname,
				image->type);
	}

	return !(nstreams > 0);
}
#else

int parse_external(struct swupdate_cfg __attribute__((__unused__)) *software,
		   const char __attribute__((__unused__)) *filename,
		   char __attribute__((__unused__)) **error)
{
	return -1;
}
#endif
