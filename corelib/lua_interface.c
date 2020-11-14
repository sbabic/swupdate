/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lua_util.h"
#include "util.h"
#include "handler.h"
#include "bootloader.h"
#include "progress.h"

#define LUA_TYPE_PEMBSCR 1
#define LUA_TYPE_HANDLER 2

#if defined(CONFIG_EMBEDDED_LUA_HANDLER)
extern const char EMBEDDED_LUA_SRC_START[];
extern const char EMBEDDED_LUA_SRC_END[];
#endif

#define LUA_PUSH_IMG_STRING(img, attr, field)  do { \
	lua_pushstring(L, attr);		\
	lua_pushstring(L, img->field);		\
	lua_settable(L, -3);			\
} while (0)

#define LUA_PUSH_IMG_STRING_VALUE(img, attr, value)  do { \
	lua_pushstring(L, attr);		\
	lua_pushstring(L, value);		\
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
static int l_call_handler(lua_State *L);
#endif
static void image2table(lua_State* L, struct img_type *img);
static void table2image(lua_State* L, struct img_type *img);
static void update_table(lua_State* L, struct img_type *img);
static int luaopen_swupdate(lua_State *L);

#ifdef CONFIG_HANDLER_IN_LUA
static bool is_type(lua_State *L, uintptr_t type)
{
	lua_getglobal(L, "SWUPDATE_LUA_TYPE");
	bool ret = lua_touserdata(L, -1) == (void*)type ? true : false;
	lua_pop(L, 1);
	return ret;
}
#endif

static void lua_dump_table(lua_State *L, char *str, struct img_type *img, const char *key)
{
	/* Stack: table, ... */
	lua_pushnil(L);
	/* Stack: nil, table, ... */
	if (!lua_istable(L, -2)) {
		return;
	}
	while (lua_next(L, -2)) {
		/* Stack: value, key, table, ... */
		lua_pushvalue(L, -2);
		/* Stack: key, value, key, table, ... */
		switch(lua_type(L, -2)) {
			case LUA_TSTRING:
			case LUA_TNUMBER:
				TRACE("%s %s = %s", str,
					lua_tostring(L, -1),
					lua_tostring(L, -2));
				if (img) {
					TRACE("Inserting property %s = %s",
							key ? key : lua_tostring(L, -1),
							lua_tostring(L, -2));
					dict_insert_value(&img->properties,
							key ? key : lua_tostring(L, -1),
							lua_tostring(L, -2));
				}
				break;
			case LUA_TFUNCTION:
				TRACE("%s %s()", str,
					lua_tostring(L, -1));
				break;
			case LUA_TTABLE: {
				char *s;
				char *propkey;

				if (asprintf(&propkey, "%s", lua_tostring(L, -1)) == ENOMEM_ASPRINTF) {
					TRACE("Out of memory, dump stopped");
					break;
				}

				if (asprintf(&s, "%s %s:", str, propkey) != ENOMEM_ASPRINTF) {
					lua_pushvalue(L, -2);
					lua_dump_table(L, s, img, propkey);
					lua_pop(L, 1);
					free(s);
				}
				free(propkey);
				break;
			}
			case LUA_TBOOLEAN:
				TRACE("%s %s = %s", str,
					lua_tostring(L, -1),
					(lua_toboolean(L, -2) ? "true" : "false"));
				if (img)
					dict_insert_value(&img->properties, str,
						(lua_toboolean(L, -2) ? "true" : "false"));
				break;
			default:
				TRACE("%s %s = <unparsed type>", str,
					lua_tostring(L, -1));
		}
		lua_pop(L, 2);
		/* Stack: key, table, ... */
	}
	/* Stack: table, ... */
}

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
				char *s;

				if (asprintf(&s, "(%d) [table ]", i) != ENOMEM_ASPRINTF) {
					lua_pushvalue(L, i);
					lua_dump_table(L, s, NULL, NULL);
					lua_pop(L, 1);
					free(s);
				}
				break;
			}
			default: {
				TRACE("(%d) [      ] unparsed type %s", i, lua_typename(L, t));
				break;
			}
		}
	}
}

int run_lua_script(const char *script, const char *function, char *parms)
{
	int ret;
	const char *output;

	lua_State *L = luaL_newstate(); /* opens Lua */
	luaL_openlibs(L); /* opens the standard libraries */
	luaL_requiref(L, "swupdate", luaopen_swupdate, 1 );

	if (luaL_loadfile(L, script)) {
		ERROR("ERROR loading %s", script);
		lua_close(L);
		return -1;
	}

	ret = lua_pcall(L, 0, 0, 0);
	if (ret) {
		LUAstackDump(L);
		ERROR("ERROR preparing Lua script %s %d",
			script, ret);
		lua_close(L);
		return -1;
	}

	lua_getglobal(L, function);
	if(!lua_isfunction(L,lua_gettop(L))) {
		lua_close(L);
		TRACE("Script : no %s in %s script, exiting", function, script);
		return 0;
	}

	/* passing arguments */
	lua_pushstring(L, parms);

	if (lua_pcall(L, 1, 2, 0)) {
		LUAstackDump(L);
		ERROR("ERROR Calling Lua script %s", script);
		lua_close(L);
		return -1;
	}

	ret = -1;

	if (lua_type(L, -2) == LUA_TBOOLEAN) {
		TRACE("LUA Exit: is boolean %d", lua_toboolean(L, -2));
		ret = lua_toboolean(L, -2) ? 0 : 1;
	}

	if (lua_type(L, -1) == LUA_TSTRING) {
		output = lua_tostring(L, -1);
		TRACE("Script output: %s script end", output);
	}

	lua_close(L);

	return ret;
}

/**
 * @brief convert an image description struct to a lua table
 *
 * @param L [inout] the Lua stack
 * @param software [in] the software struct
 */

static void lua_string_to_img(struct img_type *img, const char *key,
	       const char *value)
{
	const char offset[] = "offset";
	char seek_str[MAX_SEEK_STRING_SIZE];

	if (!strcmp(key, "compressed")) {
		if (!strcmp(value, "zlib")) {
			img->compressed = COMPRESSED_ZLIB;
		} else if (!strcmp(value, "zstd")) {
			img->compressed = COMPRESSED_ZSTD;
		} else {
			ERROR("compressed argument: '%s' invalid", value);
			img->compressed = COMPRESSED_FALSE;
		}
	}
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
		strncpy(img->mtdname, value,
			sizeof(img->mtdname));
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
	if (!strcmp(key, "ivt"))
		strncpy(img->ivt_ascii, value,
			sizeof(img->ivt_ascii));

	if (!strncmp(key, offset, sizeof(offset))) {
		strncpy(seek_str, value,
			sizeof(seek_str));
		/* convert the offset handling multiplicative suffixes */
		img->seek = ustrtoull(seek_str, 0);
		if (errno){
			ERROR("offset argument: ustrtoull failed");
		}
	}
}


static void lua_bool_to_img(struct img_type *img, const char *key,
	       bool val)
{
	if (!strcmp(key, "compressed"))
		img->compressed = (bool)val;
	if (!strcmp(key, "installed_directly"))
		img->install_directly = (bool)val;
	if (!strcmp(key, "install_if_different"))
		img->id.install_if_different = (bool)val;
	if (!strcmp(key, "install_if_higher"))
		img->id.install_if_higher = (bool)val;
	if (!strcmp(key, "encrypted"))
		img->is_encrypted = (bool)val;
	if (!strcmp(key, "partition"))
		img->is_partitioner = (bool)val;
	if (!strcmp(key, "script"))
		img->is_script = (bool)val;
}

static void lua_number_to_img(struct img_type *img, const char *key,
	       double val)
{
	if (!strcmp(key, "offset"))
		img->seek = (unsigned long long)val;
	if (!strcmp(key, "size"))
		img->size = (long long)val;
	if (!strcmp(key, "checksum"))
		img->checksum = (unsigned int)val;
	if (!strcmp(key, "skip"))
		img->skip = (unsigned int)val;
}

#ifdef CONFIG_HANDLER_IN_LUA
static int l_copy2file(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checktype(L, 2, LUA_TSTRING);

	int fdout = openfileoutput(lua_tostring(L, 2));
	lua_pop(L, 1);
	if (fdout < 0) {
		lua_pop(L, 1);
		lua_pushinteger(L, -1);
		lua_pushstring(L, strerror(errno));
		return 2;
	}

	struct img_type img = {};
	uint32_t checksum = 0;

	table2image(L, &img);
	int ret = copyfile(img.fdin,
				 &fdout,
				 img.size,
				 (unsigned long *)&img.offset,
				 img.seek,
				 0, /* no skip */
				 img.compressed,
				 &checksum,
				 img.sha256,
				 img.is_encrypted,
				 img.ivt_ascii,
				 NULL);
	update_table(L, &img);
	lua_pop(L, 1);

	if (ret < 0) {
		lua_pushinteger(L, -1);
		lua_pushstring(L, strerror(errno));
		goto copyfile_exit;
	}
	if ((img.checksum != 0) && (checksum != img.checksum)) {
		lua_pushinteger(L, -1);
		lua_pushfstring(L, "Checksums WRONG! Computed 0x%d, should be 0x%d\n",
						checksum, img.checksum);
		goto copyfile_exit;
	}

	lua_pushinteger(L, 0);
	lua_pushnil(L);

copyfile_exit:
	close(fdout);
	return 2;
}

static int istream_read_callback(void *out, const void *buf, unsigned int len)
{
	lua_State* L = (lua_State*)out;
	if (len > LUAL_BUFFERSIZE) {
		ERROR("I/O buffer size is larger than Lua's buffer size %d", LUAL_BUFFERSIZE);
		return -1;
	}

	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_pushvalue(L, 2);

	luaL_Buffer lbuffer;
	luaL_buffinit(L, &lbuffer);
	char *buffer = luaL_prepbuffsize(&lbuffer, len);
	memcpy(buffer, buf, len);
	luaL_addsize(&lbuffer, len);
	luaL_pushresult(&lbuffer);
	if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
		ERROR("Lua error in callback: %s", lua_tostring(L, -1));
		lua_pop(L, 1);
		return -1;
	}
	return 0;
}

static int l_istream_read(lua_State* L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checktype(L, 2, LUA_TFUNCTION);

	struct img_type img = {};
	uint32_t checksum = 0;

	lua_pushvalue(L, 1);
	table2image(L, &img);
	lua_pop(L, 1);

	int ret = copyfile(img.fdin,
				 L,
				 img.size,
				 (unsigned long *)&img.offset,
				 img.seek,
				 0, /* no skip */
				 img.compressed,
				 &checksum,
				 img.sha256,
				 img.is_encrypted,
				 img.ivt_ascii,
				 istream_read_callback);

	lua_pop(L, 1);
	update_table(L, &img);
	lua_pop(L, 1);

	if (ret < 0) {
		lua_pushinteger(L, -1);
		lua_pushstring(L, strerror(errno));
		return 2;
	}
	if ((img.checksum != 0) && (checksum != img.checksum)) {
		lua_pushinteger(L, -1);
		lua_pushfstring(L, "Checksums WRONG! Computed 0x%d, should be 0x%d\n",
						checksum, img.checksum);
		return 2;
	}
	lua_pushinteger(L, 0);
	lua_pushnil(L);
	return 2;
}
#endif

static void update_table(lua_State* L, struct img_type *img)
{
	if (L && img) {
		struct dict_entry *property;

		luaL_checktype(L, -1, LUA_TTABLE);

		LUA_PUSH_IMG_STRING(img, "name", id.name);
		LUA_PUSH_IMG_STRING(img, "version", id.version);
		LUA_PUSH_IMG_STRING(img, "filename", fname);
		LUA_PUSH_IMG_STRING(img, "volume", volname);
		LUA_PUSH_IMG_STRING(img, "type", type);
		LUA_PUSH_IMG_STRING(img, "device", device);
		LUA_PUSH_IMG_STRING(img, "path", path);
		LUA_PUSH_IMG_STRING(img, "mtdname", mtdname);
		LUA_PUSH_IMG_STRING(img, "data", type_data);
		LUA_PUSH_IMG_STRING(img, "filesystem", filesystem);
		LUA_PUSH_IMG_STRING(img, "ivt", ivt_ascii);

		LUA_PUSH_IMG_BOOL(img, "installed_directly", install_directly);
		LUA_PUSH_IMG_BOOL(img, "install_if_different", id.install_if_different);
		LUA_PUSH_IMG_BOOL(img, "install_if_higher", id.install_if_higher);
		LUA_PUSH_IMG_BOOL(img, "encrypted", is_encrypted);
		LUA_PUSH_IMG_BOOL(img, "partition", is_partitioner);
		LUA_PUSH_IMG_BOOL(img, "script", is_script);

		LUA_PUSH_IMG_NUMBER(img, "offset", seek);
		LUA_PUSH_IMG_NUMBER(img, "size", size);
		LUA_PUSH_IMG_NUMBER(img, "checksum", checksum);
		LUA_PUSH_IMG_NUMBER(img, "skip", skip);

		switch (img->compressed) {
			case COMPRESSED_ZLIB:
				LUA_PUSH_IMG_STRING_VALUE(img, "compressed", "zlib");
				break;
			case COMPRESSED_ZSTD:
				LUA_PUSH_IMG_STRING_VALUE(img, "compressed", "zstd");
				break;
			default:
				LUA_PUSH_IMG_BOOL(img, "compressed", compressed);
				break;
		}

		lua_pushstring(L, "properties");
		lua_newtable (L);
		LIST_FOREACH(property, &img->properties, next) {
			struct dict_list_elem *elem = LIST_FIRST(&property->list);

			lua_pushstring(L, dict_entry_get_key(property));
			if (LIST_NEXT(elem, next) == LIST_END(&property->list)) {
				lua_pushstring(L, elem->value);
			} else {
				int i = 1;

				lua_newtable (L);
				LIST_FOREACH(elem, &property->list, next) {
					lua_pushnumber(L, i++);
					lua_pushstring(L, elem->value);
					lua_settable(L, -3);
				}
			}
			lua_settable(L, -3);
		}
		lua_settable(L, -3);

#ifdef CONFIG_HANDLER_IN_LUA
		if (is_type(L, LUA_TYPE_HANDLER)) {
			lua_pushstring(L, "copy2file");
			lua_pushcfunction(L, &l_copy2file);
			lua_settable(L, -3);

			lua_pushstring(L, "read");
			lua_pushcfunction(L, &l_istream_read);
			lua_settable(L, -3);
		}
#endif

		lua_getfield(L, -1, "_private");
        LUA_PUSH_IMG_NUMBER(img, "offset", offset);
		lua_pop(L, 1);

		char *hashstring = alloca(2 * SHA256_HASH_LENGTH + 1);
		hash_to_ascii(img->sha256, hashstring);
		lua_pushstring(L, "sha256");
		lua_pushstring(L, hashstring);
		lua_settable(L, -3);
	}
}

#ifdef CONFIG_HANDLER_IN_LUA
#if LUA_VERSION_NUM > 501
static int l_istream_fclose(lua_State *L)
{
	/* closing istream is not allowed, ignore it. */
	lua_pushboolean(L, true);
	return 1;
}
#endif
#endif

static void image2table(lua_State* L, struct img_type *img)
{
	if (L && img) {
		lua_newtable (L);

		/*
		 * Create a metatable to "hide" SWUpdate-internal attributes.
		 * These are not "visible", e.g., by pairs() enumeration but
		 * may be accessed directly, knowing the attribute paths.
		 * An example is img_type's offset designating the offset
		 * in the cpio file which is used by, e.g., copyfile().
		 * While not visible in pairs() enumeration, it is directly
		 * accessible by image["_private"]["offset"].
		 * This access pattern strongly hints not to mess with the
		 * image["_private"] table values from within the Lua realm.
		 */
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_pushstring(L, "_private");
		lua_newtable(L);
		lua_settable(L, -3);
		lua_setfield(L, -2, "__index");
		lua_setmetatable(L, -2);

		update_table(L, img);

#ifdef CONFIG_HANDLER_IN_LUA
		if (is_type(L, LUA_TYPE_HANDLER)) {
			lua_getfield(L, -1, "_private");
			lua_pushstring(L, "istream");
			luaL_Stream *lstream = (luaL_Stream *)lua_newuserdata(L, sizeof(luaL_Stream));
			luaL_getmetatable(L, LUA_FILEHANDLE);
			lua_setmetatable(L, -2);
#if LUA_VERSION_NUM > 501
			lstream->closef = l_istream_fclose;
#endif
			lstream->f = fdopen(img->fdin, "r");
			if (lstream->f == NULL) {
				WARN("Cannot fdopen file descriptor %d: %s", img->fdin, strerror(errno));
			}
			lua_settable(L, -3);
			lua_pop(L, 1);
		}
#endif
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
				case LUA_TTABLE:
					if (!strcmp (lua_tostring(L, -2), "properties")) {
						dict_drop_db(&img->properties);
						lua_pushvalue(L, -1);
						lua_dump_table(L, (char*)"properties", img, NULL);
						lua_pop(L, 1);
					}
					break;
			}
			lua_pop(L, 1);
		}

		lua_getfield(L, -1, "_private");
		lua_getfield(L, -1, "offset");
		img->offset = (off_t)luaL_checknumber(L, -1);
#ifdef CONFIG_HANDLER_IN_LUA
		if (is_type(L, LUA_TYPE_HANDLER)) {
			lua_pop(L, 1);
			lua_getfield(L, -1, "istream");
			luaL_Stream *lstream = ((luaL_Stream *)luaL_checkudata(L, -1, LUA_FILEHANDLE));
			if (lstream->f == NULL) {
				img->fdin = -1;
			} else {
				img->fdin = fileno(lstream->f);
			}
		}
#endif
		lua_pop(L,2);
	}
}

/**
 * @brief function to send notifications to the recovery from Lua
 *
 * This function is exported to the Lua stack and can be called
 * from any Lua script in the same context (Stack)
 *
 * @param [in] the Lua Stack
 * @return This function returns 0 if successful and -1 if unsuccessful.
 */
static int l_notify (lua_State *L) {
	lua_Number status =  luaL_checknumber (L, 1);
	lua_Number error  =  luaL_checknumber (L, 2);
	const char *msg   =  luaL_checkstring (L, 3);

	if (msg && strlen(msg))
		notify((RECOVERY_STATUS)status, (int)error, INFOLEVEL, msg);

	lua_pop(L, 3);
	return 0;
}

static int notify_helper(lua_State *L, LOGLEVEL level)
{
	luaL_checktype(L, 1, LUA_TSTRING);
	lua_getglobal(L, "string");
	lua_pushliteral(L, "format");
	lua_gettable(L, -2);
	lua_insert(L, 1);
	lua_pop(L, 1);
	if (lua_pcall(L, lua_gettop(L) - 1, 1, 0) != LUA_OK) {
		ERROR("error while notify call: %s", lua_tostring(L, -1));
	} else {
		switch (level) {
		case ERRORLEVEL:
			ERROR("%s", lua_tostring(L, -1));
			break;
		case WARNLEVEL:
			WARN("%s", lua_tostring(L, -1));
			break;
		case INFOLEVEL:
			INFO("%s", lua_tostring(L, -1));
			break;
		case DEBUGLEVEL:
			DEBUG("%s", lua_tostring(L, -1));
			break;
		case TRACELEVEL:
			TRACE("%s", lua_tostring(L, -1));
			break;
		case OFF:
			break;
		}
	}
	lua_pop(L, 1);
	return 0;
}

int lua_notify_trace(lua_State *L) {
	return notify_helper(L, TRACELEVEL);
}

int lua_notify_error(lua_State *L) {
	return notify_helper(L, ERRORLEVEL);
}

int lua_notify_info(lua_State *L) {
	return notify_helper(L, INFOLEVEL);
}

int lua_notify_warn(lua_State *L)
{
	return notify_helper(L, WARNLEVEL);
}

int lua_notify_debug(lua_State *L)
{
	return notify_helper(L, DEBUGLEVEL);
}

static int l_mount(lua_State *L) {
	const char *device = luaL_checkstring(L, 1);
	const char *filesystem = luaL_checkstring(L, 2);
	char *target;

	if (!device || !strlen(device) || !filesystem || !strlen(filesystem))
		goto l_mount_exit;

	if (asprintf(&target, "%s%sXXXXXX", get_tmpdir(), DATADST_DIR_SUFFIX) == -1) {
		TRACE("Unable to allocate memory");
		goto l_mount_exit;
	}

	if (!mkdtemp(target)) {
		TRACE("Unable to create a unique temporary directory %s: %s",
			target, strerror(errno));
		goto l_mount_free_exit;
	}

	if (swupdate_mount(device, target, filesystem) == -1) {
		TRACE("Device %s with filesystem %s cannot be mounted: %s",
			device, filesystem, strerror(errno));
		goto l_mount_rmdir_exit;
	}

	lua_pop(L, 2);
	lua_pushstring(L, target);

	free(target);

	return 1;

l_mount_rmdir_exit:
	rmdir(target);
l_mount_free_exit:
	free(target);
l_mount_exit:
	lua_pop(L, 2);
	lua_pushnil(L);
	return 1;
}

static int l_umount(lua_State *L) {
	const char *target = luaL_checkstring(L, 1);

	if (swupdate_umount(target) == -1) {
		TRACE("Unable to unmount %s: %s", target, strerror(errno));
		goto l_umount_exit;
	}

	if (rmdir(target) == -1) {
		TRACE("Unable to remove directory %s: %s", target, strerror(errno));
		goto l_umount_exit;
	}

	lua_pop(L, 1);
	lua_pushboolean(L, true);

	return 1;

l_umount_exit:
	lua_pop(L, 1);
	lua_pushnil(L);
	return 1;
}

static int l_get_bootenv(lua_State *L) {
	const char *name = luaL_checkstring(L, 1);
	char *value = NULL;

	if (name && strlen(name))
		value = bootloader_env_get(name);
	lua_pop(L, 1);

	lua_pushstring(L, value);
	free(value);

	return 1;
}

static int l_set_bootenv(lua_State *L) {
	struct dict *bootenv = (struct dict *)lua_touserdata(L, lua_upvalueindex(1));
	const char *name = luaL_checkstring(L, 1);
	const char *value = luaL_checkstring(L, 2);

	if (name && strlen(name))
		dict_set_value(bootenv, name, value ? value : "");
	lua_pop(L, 2);

	return 0;
}

static int l_get_selection(lua_State *L) {
	char tmp[SWUPDATE_GENERAL_STRING_SIZE];

	tmp[0] = '\0';
	get_install_swset(tmp, sizeof(tmp));
	lua_pushstring(L, tmp);
	tmp[0] = '\0';
	get_install_running_mode(tmp, sizeof(tmp));
	lua_pushstring(L, tmp);

	return 2;
}


#ifdef CONFIG_HANDLER_IN_LUA
static int l_get_tmpdir(lua_State *L)
{
	lua_pushstring(L, get_tmpdir());
	return 1;
}

static int l_get_tmpdir_scripts(lua_State *L)
{
	lua_pushstring(L, get_tmpdirscripts());
	return 1;
}

static int l_progress_update(lua_State *L)
{
	lua_Number percent =  luaL_checknumber (L, 1);
	swupdate_progress_update((unsigned int) percent);
	return 0;
}
#endif

/**
 * @brief array with the function which are exported to Lua
 */
static const luaL_Reg l_swupdate[] = {
        { "notify", l_notify },
        { "error", lua_notify_error },
        { "trace", lua_notify_trace },
        { "info", lua_notify_info },
        { "warn", lua_notify_warn },
        { "debug", lua_notify_debug },
        { "mount", l_mount },
        { "umount", l_umount },
        { NULL, NULL }
};

static const luaL_Reg l_swupdate_bootenv[] = {
        { "get_bootenv", l_get_bootenv },
        { "set_bootenv", l_set_bootenv },
        { "get_selection", l_get_selection },
        { NULL, NULL }
};

#ifdef CONFIG_HANDLER_IN_LUA
static const luaL_Reg l_swupdate_handler[] = {
        { "register_handler", l_register_handler },
        { "call_handler", l_call_handler },
        { "tmpdirscripts", l_get_tmpdir_scripts },
        { "tmpdir", l_get_tmpdir },
	{ "progress_update", l_progress_update },
        { NULL, NULL }
};
#endif

static void lua_push_enum(lua_State *L, const char *name, int value)
{
	lua_pushstring(L, name);
	lua_pushnumber(L, (lua_Number) value );
	lua_settable(L, -3);
}

/**
 * @brief function to register the swupdate package in the Lua Stack
 *
 * @param [in] the Lua Stack
 * @return 1 (nr. of results on stack, the 'swupdate' module table)
 */
static int luaopen_swupdate(lua_State *L)
{
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
	if (is_type(L, LUA_TYPE_HANDLER)) {
		/* register handler-specific functions to swupdate module table. */
		luaL_setfuncs(L, l_swupdate_handler, 0);

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

		lua_pushstring(L, "handler");
		lua_newtable (L);
		struct installer_handler *hnd;
		while ((hnd = get_next_handler()) != NULL) {
			lua_pushinteger(L, 1);
			lua_setfield(L, -2, hnd->desc);
		}
		lua_settable(L, -3);
	}
#endif

	return 1;
}

#ifdef CONFIG_HANDLER_IN_LUA
static lua_State *gL = NULL;

/**
 * @brief wrapper to call the Lua function
 *
 * The reference to the Lua function is stored in the registry table.
 * To access the reference the index into this table is required. The
 * index is stored in the void* data pointer. This is due to the fact
 * that c can not store a direct reference to a Lua object.
 *
 * @param sw [in] software struct which contains all installable images
 * @param index [in] defines which image have to be installed
 * @param unused [in] unused in this context
 * @param data [in] pointer to the index in the Lua registry for the function
 * @return This function returns 0 if successful and -1 if unsuccessful.
 */
static int l_handler_wrapper(struct img_type *img, void *data) {
	int res = 0;
	lua_Number result;
	int l_func_ref;

	if (!gL || !img || !data) {
		return -1;
	}

	if (img->bootloader) {
		lua_getglobal(gL, "swupdate");
		if (!lua_istable(gL, -1)) {
			ERROR("Lua stack corrupted.");
			return -1;
		}
		lua_pushlightuserdata(gL, (void *)img->bootloader);
		luaL_setfuncs(gL, l_swupdate_bootenv, 1);
		lua_pop(gL, 1);
	}

	l_func_ref = *((int*)data);
	/* get the callback function */
	lua_rawgeti(gL, LUA_REGISTRYINDEX, l_func_ref );
	image2table(gL, img);

	if (LUA_OK != (res = lua_pcall(gL, 1, 1, 0))) {
		ERROR("Error %d while executing the Lua callback: %s",
			  res, lua_tostring(gL, -1));
		return -1;
	}

	 /* retrieve result */
	if (!lua_isnumber(gL, -1)) {
		printf(" Lua Handler must return a number");
		return -1;
	}

	result = lua_tonumber(gL, -1);
	lua_pop(gL, 1);
	TRACE("[Lua handler] returned: %d",(int)result);

	return (int) result;
}

/**
 * @brief function to register a callback from Lua
 *
 * This function is exported to the Lua stack and can be called
 * from any Lua script in the same context (Stack)
 *
 * @param [in] the Lua Stack
 * @return This function returns 0 values back to Lua.
 */
static int l_register_handler( lua_State *L ) {
	int *l_func_ref = malloc(sizeof(int));
	if(!l_func_ref) {
		ERROR("Lua handler: unable to allocate memory");
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

static int l_call_handler(lua_State *L)
{
	struct installer_handler *hnd;
	struct img_type img = {};
	char *orighndtype = NULL;
	char *msg = NULL;
	int ret = 0;

	luaL_checktype(L, 1, LUA_TSTRING);
	luaL_checktype(L, 2, LUA_TTABLE);

	table2image(L, &img);
	if ((orighndtype = strndupa(img.type, sizeof(img.type))) == NULL) {
		lua_pop(L, 2);
		lua_pushnumber(L, 1);
		lua_pushstring(L, "Error allocating memory");
		return 2;
	}
	strlcpy(img.type, lua_tostring(L, 1), sizeof(img.type));

	if ((hnd = find_handler(&img)) == NULL) {
		if (asprintf(&msg, "Image type %s not supported!", img.type) == -1) {
			msg = NULL;
		}
		ret = 1;
		goto call_handler_exit;
	}
	if ((hnd->installer(&img, hnd->data)) != 0) {
		if (asprintf(&msg, "Executing handler %s failed!", hnd->desc) == -1) {
			msg = NULL;
		}
		ret = 1;
		goto call_handler_exit;
	}

call_handler_exit:
	strlcpy(img.type, orighndtype, sizeof(img.type));
	update_table(L, &img);
	lua_pop(L, 2);
	lua_pushnumber(L, ret);
	if (msg != NULL) {
		lua_pushstring(L, msg);
		free(msg);
	} else {
		lua_pushnil(L);
	}
	return 2;
}

int lua_handlers_init(void)
{
	int ret = -1;

	gL = luaL_newstate();
	if (gL) {
		/* prime gL as LUA_TYPE_HANDLER */
		lua_pushlightuserdata(gL, (void*)LUA_TYPE_HANDLER);
		lua_setglobal(gL, "SWUPDATE_LUA_TYPE");
		/* load standard libraries */
		luaL_openlibs(gL);
		luaL_requiref( gL, "swupdate", luaopen_swupdate, 1 );
		lua_pop(gL, 1); /* remove unused copy left on stack */
		/* try to load Lua handlers for the swupdate system */
#if defined(CONFIG_EMBEDDED_LUA_HANDLER)
		if ((ret = (luaL_loadbuffer(gL, EMBEDDED_LUA_SRC_START, EMBEDDED_LUA_SRC_END-EMBEDDED_LUA_SRC_START, "LuaHandler") ||
					lua_pcall(gL, 0, LUA_MULTRET, 0))) != 0) {
			INFO("No compiled-in Lua handler(s) found.");
			TRACE("Lua exception:\n%s", lua_tostring(gL, -1));
			lua_pop(gL, 1);
		} else {
			INFO("Compiled-in Lua handler(s) found and loaded.");
		}
#else
		if ((ret = luaL_dostring(gL, "require (\"swupdate_handlers\")")) != 0) {
			INFO("No Lua handler(s) found.");
			if (luaL_dostring(gL, "return package.path:gsub('?','swupdate_handlers'):gsub(';','\\0')") == 0) {
				const char *paths = lua_tostring(gL, -2);
				for (int i=lua_tonumber(gL, -1); i >= 0; i--) {
					TRACE("\t%s", paths);
					paths += strlen(paths) + 1;
				}
				lua_pop(gL, 2);
			}
		} else {
			INFO("Lua handler(s) found.");
		}
#endif
	} else	{
		WARN("Unable to register Lua context for callbacks");
	}

	return ret;
}
#else
int lua_handlers_init(void) {return 0;}
#endif

lua_State *lua_parser_init(const char *buf, struct dict *bootenv)
{
	lua_State *L = luaL_newstate(); /* opens Lua */

	if (!L)
		return NULL;

	lua_pushlightuserdata(L, (void*)LUA_TYPE_PEMBSCR);
	lua_setglobal(L, "SWUPDATE_LUA_TYPE"); /* prime L as LUA_TYPE_PEMBSCR */
	luaL_openlibs(L); /* opens the standard libraries */
	luaL_requiref(L, "swupdate", luaopen_swupdate, 1 );
	lua_pushlightuserdata(L, (void *)bootenv);
	luaL_setfuncs(L, l_swupdate_bootenv, 1);
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

	ret = lua_pcall(L, 1, 2, 0);
	if (ret || !lua_isboolean(L, -2)) {
		LUAstackDump(L);
		ERROR("ERROR Calling Lua %s", fcn);
		return -1;
	}

	LUAstackDump(L);

	ret = lua_toboolean(L, -2) ? 0 : -1;

	/* Return 1 to indicate a missing (skipped) image */
	if (!ret && !lua_toboolean(L, -1))
		ret = 1;

	table2image(L, img);

	lua_pop(L, 2); /* clear stack */

	TRACE("Script returns %d", ret);

	return ret;
}
