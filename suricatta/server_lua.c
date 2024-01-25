/*
 * Author: Christian Storm
 * Copyright (C) 2022, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <lua_util.h>

#include <json-c/json_visit.h>

#include <util.h>
#include <network_ipc.h>
#include <progress_ipc.h>
#include <channel.h>
#include <channel_curl.h>
#include <parselib.h>
#include <state.h>
#include <bootloader.h>
#include <swupdate_settings.h>
#include <swupdate_dict.h>
#include "server_utils.h"
#include "suricatta/server.h"

#define CONFIG_SECTION "suricatta"

#if defined(CONFIG_EMBEDDED_SURICATTA_LUA)
extern const char EMBEDDED_SURICATTA_LUA_SOURCE_START[];
extern const char EMBEDDED_SURICATTA_LUA_SOURCE_END[];
#endif

extern channel_op_res_t channel_curl_init(void);


/*
 * struct for passing a channel closure's C data via the Lua stack
 * to other C functions not in the channel closure's domain.
 */
typedef struct {
	channel_data_t *channel_data;
	channel_t *channel;
} udchannel;


/*
 * Global default channel options.
 */
static channel_data_t channel_data_defaults = {
	.retry_sleep = CHANNEL_DEFAULT_RESUME_DELAY,
	.retries = CHANNEL_DEFAULT_RESUME_TRIES,
	.low_speed_timeout = 300,
	.format = CHANNEL_PARSE_JSON, /* just default, it can be overwritten */
	.debug = false,
#ifdef CONFIG_SURICATTA_SSL
	.usessl = true,
#endif
	.strictssl = true,
	.nocheckanswer = false,
	.nofollow = false,
	.source = SOURCE_SURICATTA,
};

/* Global Lua state for this Suricatta Lua module implementation. */
static lua_State *gL = NULL;

/*
 * 'enum' to help mapping Lua functions to
 *  (1) server interface functions (include/suricatta/server.h) and
 *  (2) callback Lua functions.
 */
#define MAP(x) SURICATTA_FUNC_ ## x,
#define SURICATTA_FUNCS \
	MAP(HAS_PENDING_ACTION)\
	MAP(INSTALL_UPDATE)\
	MAP(SEND_TARGET_DATA)\
	MAP(GET_POLLING_INTERVAL)\
	MAP(SERVER_START)\
	MAP(SERVER_STOP)\
	MAP(IPC)\
	MAP(PRINT_HELP)\
	MAP(CALLBACK_PROGRESS)\
	MAP(CALLBACK_CHECK_CANCEL)
typedef enum {
	SURICATTA_FUNCS \
	SURICATTA_FUNC_LAST
} function_t;
#undef MAP

/*
 * Lua functions up to (and including) SURICATTA_FUNC_MANDATORY
 * are mandatory to be registered. This is checked for in
 * suricatta_lua_create().
 * */
#define SURICATTA_FUNC_MANDATORY SURICATTA_FUNC_PRINT_HELP

/*
 * Function registry into which the Lua function implementations for
 * the server interface and the callback functions are stored.
 */
#define SURICATTA_FUNC_NULL -1
static int func_registry[SURICATTA_FUNC_LAST] = { SURICATTA_FUNC_NULL };


/*
 * Log array gathered via IPC from an installation in progress,
 * returned as part of the installation result.
 * */
static const char **ipc_journal = NULL;


/*
 * Data passed to the Lua callback function C wrappers
 * while installation is in-flight.
 *
 * @see check_cancel_callback()
 * @see progress_offloader_thread()
 * @see progress_collector_thread()
 * */
struct msgq_data_t {
	struct progress_msg entry;
	STAILQ_ENTRY(msgq_data_t) entries;
};
STAILQ_HEAD(messageq_head_t, msgq_data_t);
typedef struct {
	lua_State *L;
	pthread_mutex_t *lua_lock;
	pthread_t *thread_collector;
	pthread_t *thread_offloader;
	struct messageq_head_t progress_msgq;
	pthread_mutex_t *progress_msgq_lock;
	bool drain_progress_msgq;
	int lua_check_cancel_func;
	int fdout;
} callback_data_t;


/**
 * @brief Push name=value to Lua Table on stack top.
 *
 * Executes Table[name] = value for the Table on top of Lua's stack.
 *
 * @param L      The Lua state.
 * @param name   Name (key) of Table entry.
 * @param value  C value to push to Table[name].
 */
#define push_to_table(L, name, value) ({ \
	(__builtin_choose_expr(__builtin_types_compatible_p( \
		typeof(name), char[]         ), lua_pushstring, \
	 __builtin_choose_expr(__builtin_types_compatible_p( \
		typeof(name), char*          ), lua_pushstring, \
	 __builtin_choose_expr(__builtin_types_compatible_p( \
		typeof(name), int            ), lua_pushinteger, \
	 (void)0)))(L, name) \
	); \
	(__builtin_choose_expr(__builtin_types_compatible_p( \
		typeof(value), bool          ), lua_pushboolean, \
	 __builtin_choose_expr(__builtin_types_compatible_p( \
		typeof(value), unsigned int  ), lua_pushinteger, \
	 __builtin_choose_expr(__builtin_types_compatible_p( \
		typeof(value), int           ), lua_pushinteger, \
	 __builtin_choose_expr(__builtin_types_compatible_p( \
		typeof(value), long long     ), lua_pushinteger, \
	 __builtin_choose_expr(__builtin_types_compatible_p( \
		typeof(value), long          ), lua_pushinteger, \
	 __builtin_choose_expr(__builtin_types_compatible_p( \
		typeof(value), double        ), lua_pushnumber, \
	 __builtin_choose_expr(__builtin_types_compatible_p( \
		typeof(value), channel_body_t), lua_pushinteger, \
	 __builtin_choose_expr(__builtin_types_compatible_p( \
		typeof(value), char[]         ), lua_pushstring, \
	 __builtin_choose_expr(__builtin_types_compatible_p( \
		typeof(value), char*         ), lua_pushstring, \
	 __builtin_choose_expr(__builtin_types_compatible_p( \
		typeof(value), const char*   ), lua_pushstring, \
	 (void)0))))))))))(L, value) \
	); \
	lua_settable(L, -3); \
})


/**
 * @brief Push name=value and reverse-lookup to Lua Table on stack top.
 *
 * Executes for the Table on top of Lua's stack:
 *   Table[name]  = value
 *   Table[value] = name
 *
 * @param L      The Lua state.
 * @param name   Name (key) of Table entry.
 * @param value  C value to push to Table[name].
 */
#define push_enum_to_table(L, name, value) ({ \
	push_to_table(L, name, value); \
	push_to_table(L, value, name); \
})


/**
 * @brief Get value by name from Lua Table on stack top.
 *
 * The mechanics is as follows:
 * (1) Table[name] is defined,
 *     → get the value from the Lua Table on top of Lua's stack into dest.
 * (2) Table[name] is not defined
 *     (2.1) copy_dest is not given,
 *            → dest is not modified.
 *      (2.2) copy_dest is given and dest is a string
 *            → dest is strdup()'d.
 *
 * @param L          The Lua state.
 * @param name       Name (key) of Table entry.
 * @param dest       C destination for Table[key]'s value.
 * @param copy_dest  Whether or not to duplicate dest's value
 */
#define COPY_DEST true
#define get_from_table_3(L, name, dest) ({ \
	lua_getfield(L, -1, name); \
	if (!lua_isnil(L, -1)) { \
		dest = \
		(__builtin_choose_expr(__builtin_types_compatible_p( \
			typeof(dest), bool          ), lua_toboolean(L, -1), \
		 __builtin_choose_expr(__builtin_types_compatible_p( \
			typeof(dest), unsigned int  ), (unsigned int)lua_tointeger(L, -1), \
		 __builtin_choose_expr(__builtin_types_compatible_p( \
			typeof(dest), int           ), (int)lua_tointeger(L, -1), \
		 __builtin_choose_expr(__builtin_types_compatible_p( \
			typeof(dest), long          ), (long)lua_tointeger(L, -1), \
		 __builtin_choose_expr(__builtin_types_compatible_p( \
			typeof(dest), channel_body_t), lua_tointeger(L, -1), \
		 __builtin_choose_expr(__builtin_types_compatible_p( \
			typeof(dest), char*         ), strdup(lua_tostring(L, -1)), \
		 __builtin_choose_expr(__builtin_types_compatible_p( \
			typeof(dest), const char*   ), strdup(lua_tostring(L, -1)), \
		 (void)0))))))) \
		); \
	} \
	lua_pop(L, 1); \
})
#define get_from_table_4(L, name, dest, copy_dest) ({ \
	get_from_table_3(L, name, dest); \
	lua_getfield(L, -1, name); \
	if (lua_isnil(L, -1)) { \
		dest = \
		(__builtin_choose_expr(__builtin_types_compatible_p( \
			typeof(dest), char*         ), dest ? strdup(dest) : NULL, \
		 __builtin_choose_expr(__builtin_types_compatible_p( \
			typeof(dest), const char*   ), dest ? strdup(dest) : NULL, \
		 (void)0)) \
		); \
	} \
	lua_pop(L, 1); \
})
#define get_from_table_X(x, arg1, arg2, arg3, arg4, FUNC, ...)  FUNC
#define get_from_table(...) get_from_table_X(, ##__VA_ARGS__, \
					     get_from_table_4(__VA_ARGS__),\
					     get_from_table_3(__VA_ARGS__) \
					     )

/**
 * @brief Push the Lua equivalent of a JSON type to Table on stack top.
 *
 * @param  L      The Lua state.
 * @param  jsobj  The JSON object.
 * @return JSON_C_VISIT_RETURN_CONTINUE
 */
static int json_push_to_table(lua_State *L, json_object *jsobj)
{
	switch (json_object_get_type(jsobj)) {
	case json_type_string: {
		/* Unquote JSON string to push it unquoted to Lua. */
		char *trimmed = strdupa(json_object_to_json_string(jsobj));
		trimmed = trimmed[0] == '"' ? trimmed + 1 : trimmed;
		size_t len = strlen(trimmed);
		trimmed[len - 1] = trimmed[len - 1] == '"' ? '\0' : trimmed[len - 1];
		lua_pushstring(L, trimmed);
		break;
	}
	case json_type_boolean:
		lua_pushboolean(L, json_object_get_boolean(jsobj));
		break;
	case json_type_double:
		lua_pushnumber(L, json_object_get_int(jsobj));
		break;
	case json_type_int:
		lua_pushinteger(L, json_object_get_int(jsobj));
		break;
	case json_type_null:
		/* Lua has no notion of 'null', mimic it by an empty Table. */
		lua_createtable(L, 0, 0);
		break;
	case json_type_object:
	case json_type_array:
		/* Visiting array or object: just create a new Lua Table */
		lua_newtable(L);
		return JSON_C_VISIT_RETURN_CONTINUE;
	}
	lua_settable(L, -3);
	return JSON_C_VISIT_RETURN_CONTINUE;
}


/**
 * @brief Callback for generating a Lua Table from JSON object.
 *
 * Callback for json-c's json_c_visit() populating a Lua Table while
 * walking the JSON hierarchy. See json-c's json_c_visit() documentation.
 *
 * @param  jsobj         The JSON object.
 * @param  flags         Flags for the JSON object.
 * @param  parent_jsobj  The JSON object's parent, if any.
 * @param  jsobj_key     Set if visiting a JSON object ("dict").
 * @param  jsobj_index   Set if visiting a JSON array.
 * @param  userarg       The Lua state passed through.
 * @return 0, or, in case of error, -1.
 */
static int json_to_table_callback(json_object *jsobj, int flags,
				  json_object __attribute__((__unused__)) * parent_jsobj,
				  const char *jsobj_key, size_t *jsobj_index, void *userarg)
{
	lua_State *L = (lua_State *)userarg;

	if (!jsobj_key && !jsobj_index) {
		/* Neither array nor object, can be root node. */
		return JSON_C_VISIT_RETURN_CONTINUE;
	}
	if (flags == JSON_C_VISIT_SECOND) {
		/* Second visit to array or object: "commit" to Lua Table. */
		lua_settable(L, -3);
		return JSON_C_VISIT_RETURN_CONTINUE;
	}
	if (jsobj_index && !jsobj_key) {
		/* Visiting array element: push to Lua "Array" Table part. */
		lua_pushinteger(L, (int)*jsobj_index + 1);
	} else {
		/* Visiting object element: push to Lua "Dict" Table part. */
		lua_pushstring(L, jsobj_key);
	}
	return json_push_to_table(L, jsobj);
}


/**
 * @brief Create a Lua Table from JSON object.
 *
 * Creates a new Lua Table on stack top from a JSON object.
 * On error, the Lua stack is unchanged.
 *
 * @param  L          The Lua state.
 * @param  json_root  The JSON object.
 * @return true if successful, false otherwise.
 */
static bool json_to_table(lua_State *L, json_object *json_root)
{
	int stacktop = lua_gettop(L);
	lua_newtable(L);
	if (json_c_visit(json_root, 0, json_to_table_callback, L) < 0) {
		lua_settop(L, stacktop);
		return false;
	}
	return true;
}

/**
 * @brief Push true or nil to Lua stack according to result argument.
 *
 * Pushes true to the Lua stack if result is SERVER_OK.
 * Otherwise, nil is pushed onto the Lua stack.
 *
 * @param  L       The Lua state.
 * @param  result  enum value of server_op_res_t.
 */
static inline void push_result(lua_State *L, server_op_res_t result)
{
	result == SERVER_OK ? lua_pushboolean(L, true) : lua_pushnil(L);
}


/**
 * @brief Helper function testing whether a Lua function is registered.
 *
 * If registered, the function is left on top of Lua's stack.
 * Otherwise, the stack is not modified.
 *
 * @param  L     The Lua state.
 * @param  func  The Lua function to call, mapped to by the function_t enum.
 * @return true, or, if func is not registered, false.
 */
static bool push_registered_lua_func(lua_State *L, function_t func)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, func_registry[func]);
	if (lua_isfunction(L, -1)) {
		return true;
	}
	lua_pop(L, 1);
	return false;
}


/**
 * @brief Wrapper to call a registered Lua function.
 *
 * The reference to the Lua function to call is stored in Lua's registry table.
 * The required index to the Lua function is given in the 'func' parameter.
 * The arguments as counted in the parameter 'numargs' must be pushed onto the
 * Lua stack prior to calling this function.
 *
 * @param  L        The Lua state.
 * @param  func     The Lua function to call, mapped to by the function_t enum.
 * @param  numargs  The number of arguments for the Lua function.
 * @return func's (int) return code, or, in case of error, -1.
 */
static int call_lua_func(lua_State *L, function_t func, int numargs)
{
	#define MAP(x) #x,
	static const char *const function_names[] = { SURICATTA_FUNCS };
	#undef MAP

	if (!push_registered_lua_func(L, func)) {
		/*
		 * Optional (callback) function is not registered,
		 * return no error in this case.
		 * Mandatory functions' registration is checked in
		 * suricatta_lua_create().
		 * */
		return 0;
	}

	/*
	 * Move the function below its arguments on stack,
	 * satisfying the lua_pcall() protocol.
	 * */
	lua_insert(L, -(numargs + 1));

	int top = lua_gettop(L) - numargs - 1;
	if (lua_pcall(L, numargs, LUA_MULTRET, 0) != LUA_OK) {
		ERROR("Error executing Lua function %s: %s", function_names[func],
		      lua_tostring(L, -1));
		lua_pop(L, 1);
		return -1;
	}
	if (top == lua_gettop(L)) {
		WARN("Missing return code from Lua function %s, assuming FAILURE.",
		     function_names[func]);
		return -1;
	}
	int result = (int)luaL_checkinteger(L, -1);
	lua_pop(L, 1);
	return result;
}


/**
 * @brief Helper function properly mapping call_lua_func() to server_op_res_t.
 *
 * Note: As there is no size defined in server_op_res_t such as
 * a SERVER_LAST_CODE, any int result value > 0 is simply casted
 * to server_op_res_t and assumed to be in the enum's "range".
 *
 * @param  result  int return code of call_lua_func().
 * @return enum value of server_op_res_t.
 */
static inline server_op_res_t map_lua_result(int result)
{
	if (result < 0) {
		return SERVER_EERR;
	}
	return (server_op_res_t)result;
}


/**
 * @brief Register a server interface's or callback Lua function.
 *
 * @param  [Lua] Lua function "pointer".
 * @param  [Lua] Lua function type "enum" value of function_t.
 * @return [Lua] True, or, in case of error, nil.
 */
static int register_lua_func(lua_State *L)
{
	luaL_checktype(L, -2, LUA_TFUNCTION);
	function_t func = luaL_checknumber(L, -1);
	if (func >= SURICATTA_FUNC_LAST) {
		ERROR("Illegal function selector given.");
		lua_pushnil(L);
		lua_pop(L, 2);
	} else {
		lua_pop(L, 1);
		func_registry[func] = luaL_ref(L, LUA_REGISTRYINDEX);
		lua_pushboolean(L, true);
	}
	return 1;
}


/**
 * @brief Push channel options Table onto Lua stack.
 *
 * Creates a new table on the Lua stack and pushes channel option to it.
 *
 * @param L              The Lua state.
 * @param channel_data   channel_data_t to fill Lua Table with.
 *                       See include/channel_curl.h and lua_channel_open().
 * @return void
 */
static void channel_push_options(lua_State *L, channel_data_t *channel_data)
{
	lua_newtable(L);
	push_to_table(L, "url",                channel_data->url);
	push_to_table(L, "cached_file",        channel_data->cached_file);
	push_to_table(L, "auth",               channel_data->auth);
	push_to_table(L, "request_body",       channel_data->request_body);
	push_to_table(L, "iface",              channel_data->iface);
	push_to_table(L, "dry_run",            channel_data->dry_run);
	push_to_table(L, "cafile",             channel_data->cafile);
	push_to_table(L, "sslkey",             channel_data->sslkey);
	push_to_table(L, "sslcert",            channel_data->sslcert);
	push_to_table(L, "ciphers",            channel_data->ciphers);
	if (channel_data->proxy && channel_data->proxy == USE_PROXY_ENV) {
		push_to_table(L, "proxy",      "");
	} else {
		push_to_table(L, "proxy",      channel_data->proxy);
	}
	push_to_table(L, "info",               channel_data->info);
	push_to_table(L, "auth_token",         channel_data->auth_token);
	push_to_table(L, "content_type",       channel_data->content_type);
	push_to_table(L, "retry_sleep",        channel_data->retry_sleep);
	push_to_table(L, "method",             channel_data->method);
	push_to_table(L, "retries",            channel_data->retries);
	push_to_table(L, "low_speed_timeout",  channel_data->low_speed_timeout);
	push_to_table(L, "connection_timeout", channel_data->connection_timeout);
	push_to_table(L, "format",             channel_data->format);
	push_to_table(L, "debug",              channel_data->debug);
	push_to_table(L, "usessl",             channel_data->usessl);
	push_to_table(L, "strictssl",          channel_data->strictssl);
	push_to_table(L, "nocheckanswer",      channel_data->nocheckanswer);
	push_to_table(L, "nofollow",           channel_data->nofollow);
	push_to_table(L, "max_download_speed", channel_data->max_download_speed);
}


/**
 * @brief Set channel options from Lua Table on stack top.
 *
 * Duplicate (COPY_DEST) channel_data's value if the channel option
 * is not set in the Table but is set in channel_data.
 * channel_free_options() will free() these values.
 *
 * @param L             The Lua state.
 * @param channel_data  channel_data_t to fill from Lua Table.
 *                      See include/channel_curl.h and lua_channel_open().
 */
static void channel_set_options(lua_State *L, channel_data_t *channel_data)
{
	get_from_table(L, "url",                channel_data->url, COPY_DEST);
	get_from_table(L, "cached_file",        channel_data->cached_file, COPY_DEST);
	get_from_table(L, "auth",               channel_data->auth, COPY_DEST);
	get_from_table(L, "request_body",       channel_data->request_body, COPY_DEST);
	get_from_table(L, "iface",              channel_data->iface, COPY_DEST);
	get_from_table(L, "dry_run",            channel_data->dry_run);
	get_from_table(L, "cafile",             channel_data->cafile, COPY_DEST);
	get_from_table(L, "sslkey",             channel_data->sslkey, COPY_DEST);
	get_from_table(L, "sslcert",            channel_data->sslcert, COPY_DEST);
	get_from_table(L, "ciphers",            channel_data->ciphers, COPY_DEST);
	get_from_table(L, "info",               channel_data->info, COPY_DEST);
	get_from_table(L, "auth_token",         channel_data->auth_token, COPY_DEST);
	get_from_table(L, "content_type",       channel_data->content_type, COPY_DEST);
	get_from_table(L, "retry_sleep",        channel_data->retry_sleep);
	get_from_table(L, "method",             channel_data->method);
	get_from_table(L, "retries",            channel_data->retries);
	get_from_table(L, "low_speed_timeout",  channel_data->low_speed_timeout);
	get_from_table(L, "connection_timeout", channel_data->connection_timeout);
	get_from_table(L, "format",             channel_data->format);
	get_from_table(L, "debug",              channel_data->debug);
	get_from_table(L, "usessl",             channel_data->usessl);
	get_from_table(L, "strictssl",          channel_data->strictssl);
	get_from_table(L, "nocheckanswer",      channel_data->nocheckanswer);
	get_from_table(L, "nofollow",           channel_data->nofollow);
	char* max_download_speed = NULL;
	get_from_table(L, "max_download_speed", max_download_speed);
	if (max_download_speed) {
		channel_data->max_download_speed = (unsigned int)ustrtoull(max_download_speed, NULL, 10);
		free(max_download_speed);
	}
	lua_getfield(L, -1, "proxy");
	if (lua_isstring(L, -1)) {
		channel_data->proxy = strnlen(lua_tostring(L, -1), 1) > 0
			? strdup(lua_tostring(L, -1)) : USE_PROXY_ENV;
	} else {
		channel_data->proxy = NULL;
	}
	lua_pop(L, 1);
}


/**
 * @brief Free set channel options.
 *
 * Free memory allocated by channel_set_options() to (string) options.
 *
 * @param channel_data  channel_data_t as in include/channel_curl.h.
 */
static void channel_free_options(channel_data_t *channel_data)
{
	/* Note: If ptr is NULL, no operation is performed as per MALLOC(3). */
	free(channel_data->url);
	free(channel_data->cached_file);
	free(channel_data->auth);
	free(channel_data->request_body);
	free(channel_data->auth_token);
	free((void *)channel_data->content_type);
	free(channel_data->iface);
	free(channel_data->cafile);
	free(channel_data->sslkey);
	free(channel_data->sslcert);
	free(channel_data->ciphers);
	if (channel_data->proxy && channel_data->proxy != USE_PROXY_ENV) {
		free(channel_data->proxy);
	}
	free(channel_data->info);
}


/**
 * @brief Set HTTP headers from Lua Table on stack's top.
 *
 * @param  L          The Lua state.
 * @param  headers    Pointer to the headers dict.
 * @param  tablename  Stack-top Lua Table's header Subtable name.
 * @return true if headers set, false otherwise.
 */
static bool channel_set_header_options(lua_State *L, struct dict *headers,
				       const char *tablename)
{
	lua_getfield(L, -1, tablename);
	if (lua_isnil(L, -1) || lua_type(L, -1) != LUA_TTABLE) {
		lua_pop(L, 1);
		return false;
	}
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		if (lua_type(L, -1) == LUA_TSTRING &&
		    lua_type(L, -2) == LUA_TSTRING) {
			(void)dict_set_value(headers,
					     lua_tostring(L, -2),
					     lua_tostring(L, -1));
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return true;
}


/**
 * @brief Helper function actually executing a channel operation.
 *
 * See lua_channel_get() and lua_channel_put() for Lua
 * parameter and return values.
 *
 * @param L   The Lua state.
 * @param op  What channel operation to perform.
 */
static int channel_do_operation(lua_State *L, channel_method_t op)
{
	luaL_checktype(L, -1, LUA_TTABLE);
	udchannel *udc = (udchannel *)lua_touserdata(L, lua_upvalueindex(1));

	if (!udc->channel) {
		ERROR("Called GET/PUT channel operation on a closed channel.");
		lua_pushnil(L);
		lua_pushinteger(L, SERVER_EINIT);
		lua_newtable(L);
		return 3;
	}

	__attribute__((cleanup(channel_free_options))) channel_data_t channel_data = { 0 };
	(void)memcpy(&channel_data, udc->channel_data, sizeof(*udc->channel_data));
	channel_set_options(L, &channel_data);

	struct dict header_send;
	LIST_INIT(&header_send);
	/* Set HTTP headers as specified while channel creation. */
	if (udc->channel_data->headers_to_send) {
		struct dict_entry *entry;
		LIST_FOREACH(entry, udc->channel_data->headers_to_send, next) {
			(void)dict_insert_value(&header_send,
						dict_entry_get_key(entry),
						dict_entry_get_value(entry));
		}

	}
	/* Set HTTP headers as specified for this operation. */
	(void)channel_set_header_options(L, &header_send, "headers_to_send");
	channel_data.headers_to_send = &header_send;

	/* Setup received HTTP headers dict. */
	struct dict header_receive;
	LIST_INIT(&header_receive);
	channel_data.received_headers = &header_receive;

	lua_pop(L, 1);

	/* Perform the operation. */
	server_op_res_t result = map_channel_retcode(
	    op == CHANNEL_GET
		? udc->channel->get(udc->channel, (void *)&channel_data)
		: udc->channel->put(udc->channel, (void *)&channel_data));

	/* Assemble result for passing back to the Lua realm. */
	push_result(L, result);
	lua_pushinteger(L, result);
	lua_newtable(L);
	push_to_table(L, "http_response_code", channel_data.http_response_code);
	push_to_table(L, "format",             channel_data.format);
	if (channel_data.format == CHANNEL_PARSE_JSON) {
		lua_pushstring(L, "json_reply");
		if (!channel_data.json_reply ||
			!json_to_table(L, channel_data.json_reply)) {
			lua_pushnil(L);
		}
		lua_settable(L, -3);

		if (channel_data.json_reply &&
			json_object_put(channel_data.json_reply) != 1) {
			ERROR("JSON object should be freed but was not.");
		}
	}
	if (channel_data.format == CHANNEL_PARSE_RAW) {
		lua_pushstring(L, "raw_reply");
		if (!channel_data.raw_reply) {
			lua_pushnil(L);
		} else {
			lua_pushstring(L, channel_data.raw_reply);
			free(channel_data.raw_reply);
		}
		lua_settable(L, -3);
	}

	lua_pushstring(L, "received_headers");
	lua_newtable(L);
	if (!LIST_EMPTY(channel_data.received_headers)) {
		struct dict_entry *entry;
		LIST_FOREACH(entry, channel_data.received_headers, next) {
			lua_pushstring(L, dict_entry_get_key(entry));
			lua_pushstring(L, dict_entry_get_value(entry));
			lua_settable(L, -3);
		}
	}
	lua_settable(L, -3);
	dict_drop_db(&header_send);
	dict_drop_db(&header_receive);

	return 3;
}


/**
 * @brief GET from remote server.
 *
 * @param  [Lua] Channel options Lua Table.
 * @return [Lua] True, or, in case of error, nil.
 *         [Lua] A suricatta.status value.
 *         [Lua] Operation result Table.
 */
static int lua_channel_get(lua_State *L)
{
	return channel_do_operation(L, CHANNEL_GET);
}


/**
 * @brief PUT to remote server.
 *
 * @param  [Lua] Channel options Lua Table.
 * @return [Lua] True, or, in case of error, nil.
 *         [Lua] A suricatta.status value.
 *         [Lua] Operation result Table.
 */
static int lua_channel_put(lua_State *L)
{
	return channel_do_operation(L, CHANNEL_PUT);
}


/**
 * @brief Get SWUpdate's temporary working directory.
 *
 * @return [Lua] TMPDIR String.
 */
static int lua_suricatta_get_tmpdir(lua_State *L)
{
	lua_pushstring(L, get_tmpdir());
	return 1;
}


/**
 * @brief Callback to check for (download) cancellation on server.
 *
 * Run the Lua equivalent of the dwlwrdata callback function
 * (see channel_data_t in include/channel_curl.h) to decide
 * on whether an installation should be cancelled while the
 * download phase.
 *
 * The Lua function must have been registered as with the
 * "regular" suricatta interface functions.
 *
 * Note: The "original" suricatta Lua state is suspended here in the
 * call to suricatta.{install,download}(), so that it's safe to reuse
 * the Lua state here to check for cancellation, mutex'd with other Lua
 * callbacks.
 *
 * @return size * nmemb to continue downloading, != size * nmemb to cancel
 */
static size_t check_cancel_callback(char *streamdata, size_t size, size_t nmemb, void *data)
{
	channel_data_t *channel_data = (channel_data_t *)data;
	callback_data_t *callback_data = (callback_data_t *)channel_data->user;

	if (callback_data->lua_check_cancel_func != SURICATTA_FUNC_NULL) {
		(void)pthread_mutex_lock(callback_data->lua_lock);
		int result = call_lua_func(callback_data->L,
					   callback_data->lua_check_cancel_func, 0);
		(void)pthread_mutex_unlock(callback_data->lua_lock);
		if (result == SERVER_UPDATE_CANCELED) {
			return 0;
		}
	}

	if (callback_data->fdout != -1) {
		if (copy_write(&callback_data->fdout, streamdata, size * nmemb) != 0) {
			return 0;
		}
	}

	return size * nmemb;
}


/**
 * @brief IPC wait callback storing error messages into journal.
 *
 * Callback function for ipc_wait_for_complete() as in ipc/network_ipc.c
 * that appends error messages to the journal returned as installation result.
 *
 * @param  msg  IPC message.
 * @return 0, however not checked by ipc_wait_for_complete().
 */
static int ipc_wait_for_complete_cb(ipc_message *msg)
{
	if (strncmp(msg->data.status.desc, "ERROR", 5) == 0) {
		int lines = (ipc_journal == NULL ? 0 : count_string_array(ipc_journal)) + 2;
		const char **tmp = reallocarray(ipc_journal, lines, sizeof(char*));
		if (!tmp) {
			ERROR("Cannot (re-)allocate IPC journal memory.");
			return 0;
		}
		ipc_journal = tmp;
		ipc_journal[lines - 2] = strndup(msg->data.status.desc, PRINFOSIZE);
		ipc_journal[lines - 1] = NULL;
	}
	return 0;
}


/**
 * @brief Thread offloading collected progress messages to the server.
 *
 * Note: The "original" suricatta Lua state is suspended here in the
 * call to suricatta.{install,download}(), so that it's safe to reuse
 * the Lua state to report progress to the server, mutex'd with other
 * Lua callbacks.
 *
 * @see progress_collector_thread().
 *
 * @param  data  Pointer to a callback_data_t structure.
 */
static void *progress_offloader_thread(void *data)
{
	callback_data_t *thread_data = (callback_data_t *)data;

	while (true) {
		(void)pthread_mutex_lock(thread_data->progress_msgq_lock);
		if (STAILQ_EMPTY(&thread_data->progress_msgq)) {
			(void)pthread_mutex_unlock(thread_data->progress_msgq_lock);
			/* Accept cancellation on empty progress message queue. */
			(void)pthread_testcancel();
		} else {
			(void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
			struct msgq_data_t *qitem = STAILQ_FIRST(
			    &thread_data->progress_msgq);
			struct progress_msg *message = &qitem->entry;

			(void)pthread_mutex_lock(thread_data->lua_lock);
			lua_newtable(thread_data->L);
			push_to_table(thread_data->L, "apiversion",  message->apiversion);
			push_to_table(thread_data->L, "status",      message->status);
			push_to_table(thread_data->L, "dwl_percent", message->dwl_percent);
			push_to_table(thread_data->L, "nsteps",      message->nsteps);
			push_to_table(thread_data->L, "cur_step",    message->cur_step);
			push_to_table(thread_data->L, "cur_percent", message->cur_percent);
			push_to_table(thread_data->L, "cur_image",   (char*)message->cur_image);
			push_to_table(thread_data->L, "hnd_name",    (char*)message->hnd_name);
			push_to_table(thread_data->L, "source",      message->source);
			push_to_table(thread_data->L, "info",        (char*)message->info);
			if (message->infolen > 0) {
				lua_pushstring(thread_data->L, "jsoninfo");
				struct json_object *json_root = json_tokener_parse(
				    message->info);
				if (!json_root ||
				    !json_to_table(thread_data->L, json_root)) {
					lua_pushnil(thread_data->L);
				}
				if (json_root && json_object_put(json_root) != 1) {
					ERROR("Progress JSON object should be freed but was not.");
				}
				lua_settable(thread_data->L, -3);
			}
			STAILQ_REMOVE_HEAD(&thread_data->progress_msgq, entries);
			free(qitem);
			(void)pthread_mutex_unlock(thread_data->progress_msgq_lock);
			(void)call_lua_func(thread_data->L, SURICATTA_FUNC_CALLBACK_PROGRESS, 1);
			(void)pthread_mutex_unlock(thread_data->lua_lock);
			(void)pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

			if (thread_data->drain_progress_msgq == false) {
				/* Accept cancellation if messages mustn't be flushed completely. */
				(void)pthread_testcancel();
			}
		}
	}
	return NULL;
}


/**
 * @brief Cleanup handler for the progress messages collector thread.
 *
 * Closes the progress socket file descriptor on thread cancellation.
 *
 * @param pfd  Pointer to the progress file descriptor.
 */
static void progress_collector_thread_cleanup(void *pfd)
{
	/* Just close it, ignoring errors. */
	(void)close(*(int *)pfd);
}


/**
 * @brief Thread collecting progress messages for offload by another thread.
 *
 * @see progress_offloader_thread().
 *
 * @param  data  Pointer to a callback_data_t structure.
 */
static void *progress_collector_thread(void *data)
{
	callback_data_t *thread_data = (callback_data_t *)data;
	struct progress_msg message;
	int progress_fd = -1;

	pthread_cleanup_push(progress_collector_thread_cleanup, (void *)&progress_fd);

	while (true) {
		if (progress_fd < 0) {
			if ((progress_fd = progress_ipc_connect(true)) < 0) {
				usleep(250000);
				continue;
			}
		}

		if (progress_ipc_receive(&progress_fd, &message) <= 0) {
			continue;
		}

		/* Null-terminate progress message strings. */
		if (message.infolen > 0) {
			if (message.infolen > sizeof(message.info) - 1) {
				message.infolen = sizeof(message.info) - 1;
			}
		}
		message.info[message.infolen] = '\0';
		message.hnd_name[sizeof(message.hnd_name) - 1] = '\0';
		message.cur_image[sizeof(message.cur_image) - 1] = '\0';

		struct msgq_data_t *qentry = malloc(sizeof(struct msgq_data_t));
		(void)memcpy(&qentry->entry, &message, sizeof(struct progress_msg));
		(void)pthread_mutex_lock(thread_data->progress_msgq_lock);
		STAILQ_INSERT_TAIL(&thread_data->progress_msgq, qentry, entries);
		(void)pthread_mutex_unlock(thread_data->progress_msgq_lock);
	}

	pthread_cleanup_pop(1);
	return NULL;
}


/**
 * @brief Helper function to cancel and join (progress) threads.
 *
 * @param thread       Pointer to thread's pthread_t.
 * @param thread_name  Name of the thread.
 */
static void join_progress_threads(pthread_t *thread, const char *thread_name)
{
	if (pthread_cancel(*thread) != 0) {
		ERROR("Cannot enqueue %s thread cancellation.", thread_name);
	} else {
		void *join_result;
		if (pthread_join(*thread, &join_result) != 0) {
			ERROR("Thread join on %s thread failed!", thread_name);
		}
		if (join_result != PTHREAD_CANCELED) {
			ERROR("Thread %s terminated deliberately.", thread_name);
		}
	}
}


/**
 * @brief Installation helper doing the installation heavy lifting.
 *
 * @param  [Lua] Channel options Lua Table plus channel={channel} attribute.
 * @return [Lua] True, or, in case of error, nil.
 *         [Lua] A suricatta.status value.
 *         [Lua] Table with messages in case of error, else empty Table.
 * @see lua_suricatta_{install,download}() for parameter details.
 *
 * @param  L      The Lua state.
 * @param  fdout  File descriptor to save artifact to, or -1 for IPC.
 */
static void do_install(lua_State *L, int fdout)
{
	__attribute__((cleanup(channel_free_options))) channel_data_t channel_data = { 0 };
	callback_data_t callback_data = { .L = L, .fdout = fdout };

	lua_getfield(L, -1, "channel");
	if (!lua_istable(L, -1) || (lua_getmetatable(L, -1) == 0)) {
		ERROR("Channel table does not have metatable.");
		lua_pop(L, 2);
		goto error;
	}
	lua_getfield(L, -1, "__getchannel");
	if (!lua_isfunction(L, -1)) {
		ERROR("Channel metatable does not have __getchannel() function.");
		lua_pop(L, 4);
		goto error;
	}
	if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
		ERROR("Error processing channel metatable: %s", lua_tostring(L, -1));
		lua_pop(L, 3 + 1);
		goto error;
	}
	udchannel *udc = (udchannel *)lua_touserdata(L, -1);
	(void)memcpy(&channel_data, udc->channel_data, sizeof(*udc->channel_data));
	lua_pop(L, 3);
	channel_set_options(L, &channel_data);
	get_from_table(L, "drain_messages", callback_data.drain_progress_msgq);
	lua_pop(L, 1);

	channel_data.noipc = fdout == -1 ? false : true;
	channel_data.user = (void *)&callback_data;

	/* Global Lua state lock to synchronize the Lua callbacks functions. */
	pthread_mutex_t _lua_lock;
	if (pthread_mutex_init(&_lua_lock, NULL)) {
		ERROR("Error creating Lua state mutex.");
		lua_pop(L, 1);
		goto error;
	}
	callback_data.lua_lock = &_lua_lock;

	/* Setup Lua callback function to call in check_cancel_callback(). */
	channel_data.dwlwrdata = check_cancel_callback;
	callback_data.lua_check_cancel_func = SURICATTA_FUNC_NULL;
	if (push_registered_lua_func(L, SURICATTA_FUNC_CALLBACK_CHECK_CANCEL)) {
		lua_pop(L, 1);
		callback_data.lua_check_cancel_func = SURICATTA_FUNC_CALLBACK_CHECK_CANCEL;
	}

	/* Setup progress message handling threads and Lua callback function. */
	pthread_mutex_t _progress_msgq_lock;
	pthread_t _thread_progress_collector;
	pthread_t _thread_progress_offloader;
	if (push_registered_lua_func(L, SURICATTA_FUNC_CALLBACK_PROGRESS)) {
		lua_pop(L, 1);

		/* Initialize progress messages (tail) queue. */
		STAILQ_INIT(&callback_data.progress_msgq);

		/* Initialize progress messages (tail) queue synchronization mutex. */
		if (pthread_mutex_init(&_progress_msgq_lock, NULL)) {
			ERROR("Error creating progress message queue mutex.");
			lua_pop(L, 1);
			goto error;
		}
		callback_data.progress_msgq_lock = &_progress_msgq_lock;

		/* Spawn threads handling progress notification to server. */
		if ((pthread_create(&_thread_progress_collector, NULL,
				    progress_collector_thread,
				    &callback_data) != 0) ||
		    (pthread_create(&_thread_progress_offloader, NULL,
				    progress_offloader_thread,
				    &callback_data) != 0)) {
			ERROR("Error starting progress message handling threads.");
			lua_pop(L, 1);
			goto error;
		}
		callback_data.thread_collector = &_thread_progress_collector;
		callback_data.thread_offloader = &_thread_progress_offloader;
	}

	/* Perform the operation.... */
	server_op_res_t result = map_channel_retcode(
	    udc->channel->get_file(udc->channel, (void *)&channel_data));
	RECOVERY_STATUS iresult = (RECOVERY_STATUS)ipc_wait_for_complete(
	    ipc_wait_for_complete_cb);

	/* Clean up the progress message handling threads and queue. */
	if (callback_data.thread_collector && callback_data.thread_offloader) {
		join_progress_threads(callback_data.thread_offloader, "progress_offloader");
		join_progress_threads(callback_data.thread_collector, "progress_collector");

		while (!STAILQ_EMPTY(&callback_data.progress_msgq)) {
			struct msgq_data_t *entry = STAILQ_FIRST(
			    &callback_data.progress_msgq);
			STAILQ_REMOVE_HEAD(&callback_data.progress_msgq, entries);
			free(entry);
		}
	}

	if (result == SERVER_OK) {
		result = iresult == FAILURE ? SERVER_EERR : SERVER_OK;
	}
	push_result(L, result);
	lua_pushinteger(L, result);
	lua_newtable(L);
	if (ipc_journal) {
		const char **iter = ipc_journal;
		while (*iter != NULL) {
			lua_pushstring(L, *iter);
			lua_rawseti(L, -2, (int)lua_rawlen(L, -2) + 1);
			iter++;
		}
		free_string_array((char **)ipc_journal);
		ipc_journal = NULL;
	}

	goto done;
error:
	lua_pushnil(L);
	lua_pushinteger(L, SERVER_EINIT);
	lua_newtable(L);
done:
	if (callback_data.progress_msgq_lock) {
		if (pthread_mutex_destroy(callback_data.progress_msgq_lock) != 0) {
			ERROR("Mutex deallocation for progress message queue failed!");
		}
	}
	if (callback_data.lua_lock) {
		if (pthread_mutex_destroy(callback_data.lua_lock) != 0) {
			ERROR("Mutex deallocation for Lua state failed!");
		}
	}
}


/**
 * @brief Install an update artifact from remote server or local file.
 *
 * For installing a local update artifact file, the url Table field should
 * read file:///path/to/file.swu. For downloading *and* installing an update
 * artifact, the url should read, e.g., https://artifacts.io/update.swu.
 * Note that this file is to be deleted, if applicable, from the Lua realm.
 *
 * @param  [Lua] Channel options Lua Table plus mandatory channel={channel} attribute.
 * @return [Lua] True, or, in case of error, nil.
 *         [Lua] A suricatta.status value.
 *         [Lua] Table with messages in case of error, else empty Table.
 */
static int lua_suricatta_install(lua_State *L)
{
	luaL_checktype(L, -1, LUA_TTABLE);
	do_install(L, -1);
	return 3;
}


/**
 * @brief Download an update artifact from remote server (w/o installing it).
 *
 * @param  [Lua] Download channel options Lua Table plus mandatory channel={channel} attribute.
 * @param  [Lua] Output local file name
 * @return [Lua] True, or, in case of error, nil.
 *         [Lua] A suricatta.status value.
 *         [Lua] Table with messages in case of error, else empty Table.
 */
static int lua_suricatta_download(lua_State *L)
{
	luaL_checktype(L, -1, LUA_TSTRING);
	luaL_checktype(L, -2, LUA_TTABLE);
	DEBUG("Saving artifact to %s", luaL_checkstring(L, -1));
	int fdout = open(luaL_checkstring(L, -1), O_CREAT | O_WRONLY | O_TRUNC,
			 S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	if (fdout == -1) {
		ERROR("Cannot open %s for writing.", luaL_checkstring(L, -1));
		lua_pop(L, 2);
		lua_pushnil(L);
		lua_pushinteger(L, SERVER_EINIT);
		lua_newtable(L);
		return 3;
	}
	lua_pop(L, 1);
	do_install(L, fdout);
	(void)close(fdout);
	return 3;
}


/**
 * @brief Helper to push channel closure to Lua stack.
 *
 * Helper function to push the channel's closure variables 'channel' and
 * 'channel_data' (via udchannel) to Lua's stack to allow C functions not
 * in the channel's closure domain to access them via lua_touserdata().
 *
 * @return [Lua] udchannel.
 */
static int push_channel_closure(lua_State *L)
{
	udchannel *udc = lua_touserdata(L, lua_upvalueindex(1));
	udchannel *tgt = lua_newuserdata(L, sizeof(udchannel));
	*tgt = *udc;
	luaL_checktype(L, -1, LUA_TUSERDATA);
	return 1;
}


/**
 * @brief Helper function deallocating a channel.
 * @param channel       Channel to close.
 * @param channel_data  Channel data as per include/channel_curl.h.
 */
static void do_channel_close(channel_t *channel, channel_data_t *channel_data)
{
	if (channel) {
		(void)channel->close(channel);
		free(channel);
		channel = NULL;
	}

	if (channel_data) {
		channel_free_options(channel_data);

		if (channel_data->headers_to_send) {
			dict_drop_db(channel_data->headers_to_send);
			free(channel_data->headers_to_send);
		}

		free(channel_data);
		channel_data = NULL;
	}
}


/**
 * @brief Close channel to remote server.
 */
static int lua_channel_close(lua_State *L)
{
	udchannel *udc = (udchannel *)lua_touserdata(L, lua_upvalueindex(1));
	if (!udc->channel && !udc->channel_data) {
		ERROR("Called CLOSE operation on a closed channel.");
	}
	do_channel_close(udc->channel, udc->channel_data);
	return 0;
}


/**
 * @brief Open channel to remote server.
 *
 * @param  [Lua] Lua Table with Channel defaults.
 *               See channel_set_options() and include/channel_curl.h.
 * @return [Lua] True, or, in case of error, nil.
 *         [Lua] Channel operations Table, empty Table in case of error.
 */
static int lua_channel_open(lua_State *L)
{
	luaL_checktype(L, -1, LUA_TTABLE);

	channel_t *channel = NULL;
	channel_data_t *channel_data = malloc(sizeof(*channel_data));
	if (!channel_data) {
		ERROR("Cannot allocate channel memory.");
		goto failure;
	}
	(void)memcpy(channel_data, &channel_data_defaults, sizeof(*channel_data));

	/* Set common options selectively overridable in channel operations. */
	channel_set_options(L, channel_data);

	channel_data->headers_to_send = malloc(sizeof(struct dict));
	if (!channel_data->headers_to_send) {
		ERROR("Cannot allocate channel header memory.");
		goto failure;
	}

	/* Set global default HTTP header options for channel. */
	LIST_INIT(channel_data->headers_to_send);
	(void)channel_set_header_options(L, channel_data->headers_to_send,
					 "headers_to_send");

	lua_pop(L, 1);

	luaL_Reg lua_funcs_channel[] = {
		{ "get",   lua_channel_get   },
		{ "put",   lua_channel_put   },
		{ "close", lua_channel_close },
		{ NULL, NULL }
	};

	channel = channel_new();
	if (!channel) {
		ERROR("Cannot allocate new channel memory.");
		goto failure;
	}

	channel_op_res_t result = channel->open(channel, channel_data);
	if (result != CHANNEL_OK) {
		ERROR("Cannot open channel.");
		goto failure;
	}

	lua_pushboolean(L, true);
	lua_newtable(L);

	lua_pushstring(L, "options");
	channel_push_options(L, channel_data);
	lua_settable(L, -3);

	udchannel *udc = lua_newuserdata(L, sizeof(udchannel));
	udc->channel = channel;
	udc->channel_data = channel_data;
	lua_pushvalue(L, -1);
	lua_insert(L, -3);
	luaL_setfuncs(L, lua_funcs_channel, 1);
	lua_insert(L, -2);
	lua_pushcclosure(L, push_channel_closure, 1);
	lua_createtable(L, 0, 1);
	lua_insert(L, -2);
	lua_setfield(L, -2, "__getchannel");
	lua_setmetatable(L, -2);
	return 2;

failure:
	do_channel_close(channel, channel_data);
	lua_pushnil(L);
	lua_newtable(L);
	return 2;
}

/**
 * @brief Test whether a bootloader is currently set.
 *
 * @param  [Lua] Name of bootloader to test for being currently set.
 * @return [Lua] True if given name is the currently set bootloader, false otherwise.
 */
static int lua_bootloader_is(lua_State *L)
{
	lua_pushboolean(L, is_bootloader(luaL_checkstring(L, -1)));
	return 1;
}

/**
 * @brief Get currently set bootloader's name.
 *
 * @return [Lua] Name of currently set bootloader.
 */
static int lua_bootloader_get(lua_State *L)
{
	(void)lua_pushstring(L, get_bootloader());
	return 1;
}

/**
 * @brief Get value of a bootloader environment variable.
 *
 * @param  [Lua] Name of the bootloader environment variable to get value of.
 * @return [Lua] Value of the bootloader environment variable.
 */
static int lua_bootloader_env_get(lua_State *L)
{
	char* value = bootloader_env_get(luaL_checkstring(L, -1));
	(void)lua_pushstring(L, value);
	free(value);
	return 1;
}

/**
 * @brief Set value of a bootloader environment variable.
 *
 * @param  [Lua] Name of the bootloader environment variable to set.
 * @param  [Lua] Value to set the bootloader environment variable to.
 * @return [Lua] True, or, in case of error, nil.
 */
static int lua_bootloader_env_set(lua_State *L)
{
	bootloader_env_set(luaL_checkstring(L, -2), luaL_checkstring(L, -1)) == 0
		? lua_pushboolean(L, true)
		: lua_pushnil(L);
	return 1;
}

/**
 * @brief Drop a bootloader environment variable.
 *
 * @param  [Lua] Name of the bootloader environment variable to drop.
 * @return [Lua] True, or, in case of error, nil.
 */
static int lua_bootloader_env_unset(lua_State *L)
{
	bootloader_env_unset(luaL_checkstring(L, -1)) == 0
		? lua_pushboolean(L, true)
		: lua_pushnil(L);
	return 1;
}

/**
 * @brief Set multiple bootloader environment variables from local file.
 *
 * @param  [Lua] Path to local file in format `<variable>=<value>`.
 * @return [Lua] True, or, in case of error, nil.
 */
static int lua_bootloader_env_apply(lua_State *L)
{
	bootloader_apply_list(luaL_checkstring(L, -1)) == 0
		? lua_pushboolean(L, true)
		: lua_pushnil(L);
	return 1;
}

/**
 * @brief Get update state from persistent storage (bootloader).
 *
 * @return [Lua] One of pstate's enum values.
 */
static int lua_pstate_get(lua_State *L)
{
	update_state_t state = get_state();
	lua_pushinteger(L, is_valid_state(state) ? (int)state : STATE_ERROR);
	return 1;
}


/**
 * @brief Save update state to persistent storage (bootloader).
 *
 * @param  [Lua] One of pstate's enum values.
 * @return [Lua] True, or, in case of error, nil.
 */
static int lua_pstate_save(lua_State *L)
{
	update_state_t state = luaL_checknumber(L, -1);
	lua_pop(L, 1);
	if (!is_valid_state(state)) {
		lua_pushnil(L);
	} else {
		push_result(L, save_state(state) == 0 ? SERVER_OK : SERVER_EERR);
	}
	return 1;
}


/**
 * @brief Sleep for a specified number of seconds.
 *
 * Seconds timer resolution is sufficient for the purpose of a
 * short pause in loops. That's why handling the return value
 * (short-sleep due to signal) is deliberately ignored.
 *
 * @param  [Lua] Number of seconds to sleep.
 */
static int lua_suricatta_sleep(lua_State *L)
{
	(void)sleep((unsigned int)luaL_checkinteger(L, -1));
	return 0;
}


/**
 * @brief Register the 'suricatta' module to Lua.
 *
 * @param  L  The Lua state.
 * @return 1, i.e., the 'suricatta' module Table on stack.
 */
static int suricatta_lua_module(lua_State *L)
{
	/* Remove 'modname' string from stack. */
	lua_pop(L, 1);

	luaL_checkversion(L);
	lua_newtable(L);
	luaL_Reg lua_funcs_suricatta[] = {
		{ "sleep",      lua_suricatta_sleep      },
		{ "install",    lua_suricatta_install    },
		{ "download",   lua_suricatta_download   },
		{ "get_tmpdir", lua_suricatta_get_tmpdir },
		{ "getversion", lua_get_swupdate_version },
		{ NULL, NULL }
	};
	luaL_setfuncs(L, lua_funcs_suricatta, 0);

	lua_pushstring(L, "server");
	lua_newtable(L);
	luaL_Reg lua_funcs_server[] = {
		{ "register", register_lua_func },
		{ NULL, NULL }
	};
	luaL_setfuncs(L, lua_funcs_server, 0);
	#define MAP(x) push_to_table(L, #x, SURICATTA_FUNC_ ## x);
	SURICATTA_FUNCS
	#undef MAP
	lua_settable(L, -3);

	luaL_Reg lua_funcs_bootloader[] = {
		{ "is",  lua_bootloader_is  },
		{ "get", lua_bootloader_get },
		{ NULL, NULL }
	};
	luaL_Reg lua_funcs_bootloader_env[] = {
		{ "get",   lua_bootloader_env_get   },
		{ "set",   lua_bootloader_env_set   },
		{ "unset", lua_bootloader_env_unset },
		{ "apply", lua_bootloader_env_apply },
		{ NULL, NULL }
	};
	lua_pushstring(L, "bootloader");
	lua_newtable(L);
	luaL_setfuncs(L, lua_funcs_bootloader, 0);
	lua_pushstring(L, "bootloaders");
	lua_newtable(L);
	push_to_table(L, "EBG",   BOOTLOADER_EBG);
	push_to_table(L, "NONE",  BOOTLOADER_NONE);
	push_to_table(L, "GRUB",  BOOTLOADER_GRUB);
	push_to_table(L, "UBOOT", BOOTLOADER_UBOOT);
	lua_settable(L, -3);
	lua_pushstring(L, "env");
	lua_newtable(L);
	luaL_setfuncs(L, lua_funcs_bootloader_env, 0);
	lua_settable(L, -3);
	lua_settable(L, -3);

	lua_pushstring(L, "ipc");
	lua_newtable(L);
	lua_pushstring(L, "sourcetype");
	lua_newtable(L);
	push_to_table(L, "SOURCE_UNKNOWN", SOURCE_UNKNOWN);
	push_to_table(L, "SOURCE_WEBSERVER", SOURCE_WEBSERVER);
	push_to_table(L, "SOURCE_SURICATTA", SOURCE_SURICATTA);
	push_to_table(L, "SOURCE_DOWNLOADER", SOURCE_DOWNLOADER);
	push_to_table(L, "SOURCE_LOCAL", SOURCE_LOCAL);
	push_to_table(L, "SOURCE_CHUNKS_DOWNLOADER", SOURCE_CHUNKS_DOWNLOADER);
	lua_settable(L, -3);
	lua_pushstring(L, "RECOVERY_STATUS");
	lua_newtable(L);
	push_to_table(L, "IDLE", IDLE);
	push_to_table(L, "START", START);
	push_to_table(L, "RUN", RUN);
	push_to_table(L, "SUCCESS", SUCCESS);
	push_to_table(L, "FAILURE", FAILURE);
	push_to_table(L, "DOWNLOAD", DOWNLOAD);
	push_to_table(L, "DONE", SUBPROCESS);
	push_to_table(L, "SUBPROCESS", SUBPROCESS);
	push_to_table(L, "PROGRESS", PROGRESS);
	lua_settable(L, -3);
	lua_settable(L, -3);

	lua_pushstring(L, "status");
	lua_newtable(L);
	push_to_table(L, "OK",                  SERVER_OK);
	push_to_table(L, "EERR",                SERVER_EERR);
	push_to_table(L, "EBADMSG",             SERVER_EBADMSG);
	push_to_table(L, "EINIT",               SERVER_EINIT);
	push_to_table(L, "EACCES",              SERVER_EACCES);
	push_to_table(L, "EAGAIN",              SERVER_EAGAIN);
	push_to_table(L, "UPDATE_AVAILABLE",    SERVER_UPDATE_AVAILABLE);
	push_to_table(L, "NO_UPDATE_AVAILABLE", SERVER_NO_UPDATE_AVAILABLE);
	push_to_table(L, "UPDATE_CANCELED",     SERVER_UPDATE_CANCELED);
	push_to_table(L, "ID_REQUESTED",        SERVER_ID_REQUESTED);
	lua_settable(L, -3);

	luaL_Reg lua_funcs_pstate[] = {
		{ "get",   lua_pstate_get   },
		{ "save",  lua_pstate_save  },
		{ NULL, NULL }
	};
	lua_pushstring(L, "pstate");
	lua_newtable(L);
	luaL_setfuncs(L, lua_funcs_pstate, 0);
	push_enum_to_table(L, "OK",            STATE_OK);
	push_enum_to_table(L, "INSTALLED",     STATE_INSTALLED);
	push_enum_to_table(L, "TESTING",       STATE_TESTING);
	push_enum_to_table(L, "FAILED",        STATE_FAILED);
	push_enum_to_table(L, "NOT_AVAILABLE", STATE_NOT_AVAILABLE);
	push_enum_to_table(L, "ERROR",         STATE_ERROR);
	push_enum_to_table(L, "WAIT",          STATE_WAIT);
	push_enum_to_table(L, "IN_PROGRESS",   STATE_IN_PROGRESS);
	lua_settable(L, -3);

	luaL_Reg lua_funcs_channel[] = {
		{ "open",  lua_channel_open },
		{ NULL, NULL }
	};
	lua_pushstring(L, "channel");
	lua_newtable(L);
	luaL_setfuncs(L, lua_funcs_channel, 0);

	lua_pushstring(L, "options");
	channel_push_options(L, &channel_data_defaults);
	lua_settable(L, -3);

	lua_pushstring(L, "USE_PROXY_ENV");
	lua_pushstring(L, "");
	lua_settable(L, -3);

	lua_pushstring(L, "content");
	lua_newtable(L);
	push_to_table(L, "RAW",  CHANNEL_PARSE_RAW);
	push_to_table(L, "JSON", CHANNEL_PARSE_JSON);
	push_to_table(L, "NONE", CHANNEL_PARSE_NONE);
	lua_settable(L, -3);

	lua_pushstring(L, "method");
	lua_newtable(L);
	push_to_table(L, "GET",   CHANNEL_GET);
	push_to_table(L, "POST",  CHANNEL_POST);
	push_to_table(L, "PUT",   CHANNEL_PUT);
	push_to_table(L, "PATCH", CHANNEL_PATCH);
	lua_settable(L, -3);

	lua_settable(L, -3);

	luaL_Reg lua_funcs_notify[] = {
		{ "error",    lua_notify_error    },
		{ "trace",    lua_notify_trace    },
		{ "info",     lua_notify_info     },
		{ "warn",     lua_notify_warn     },
		{ "debug",    lua_notify_debug    },
		{ "progress", lua_notify_progress },
		{ NULL, NULL }
	};
	lua_pushstring(L, "notify");
	lua_newtable(L);
	luaL_setfuncs(L, lua_funcs_notify, 0);
	lua_settable(L, -3);

	return 1;
}


/**
 * @brief Unload and de-initialize Lua state and the Suricatta Lua module.
 */
static void suricatta_lua_destroy(void)
{
	if (!gL) {
		return;
	}
	lua_close(gL);
	gL = NULL;
}


/**
 * @brief Helper function for Lua Table nil-indexing.
 *
 * Read-accessing an absent field in a Lua Table will invoke and return
 * the result of the __index metamethod, if defined, or, if not defined
 * as per default, return nil.
 * As a consequence, in nested Lua Tables, all intermediate Tables on a
 * "path" must exist as it is the case in the following example
 *    local tbl = { a = { b = { c = { d = 23 } } } }
 *    print(tbl.a.b.c.d)
 * or an "attempt to index a nil value" error is triggered, requiring
 * existence checks for every node in the "path" to be implemented.
 *
 * For convenience, on_nil_table_index() may be registered as __index
 * metamethod to nil returning zero results, i.e., nil, if a node in
 * the Table "path" is nil without triggering the "attempt to index
 * a nil value" error.
 *
 * @param  L     The Lua state.
 * @param  [Lua] The table in which the look-up failed.
 * @param  [Lua] The key name whose look-up failed.
 * @return 0, i.e., nil
 */
static int on_nil_table_index(lua_State *L)
{
	/* Pop table and key parameters. */
	lua_pop(L, 2);
	return 0;
}


/**
 * @brief Load and initialize Lua and the Suricatta Lua module.
 *
 * @return SERVER_OK, or, in case of errors, SERVER_EINIT.
 */
static server_op_res_t suricatta_lua_create(void)
{
	if (gL) {
		TRACE("[Lua suricatta] Lua state already initialized.");
		return SERVER_OK;
	}
	if (!(gL = luaL_newstate())) {
		ERROR("Unable to register Suricatta Lua module.");
		return SERVER_EINIT;
	}
	luaL_openlibs(gL);
	luaL_requiref(gL, "suricatta", suricatta_lua_module, true);
	lua_pop(gL, 1);

	#if defined(CONFIG_EMBEDDED_SURICATTA_LUA)
	if ((luaL_loadbuffer(gL, EMBEDDED_SURICATTA_LUA_SOURCE_START,
			     EMBEDDED_SURICATTA_LUA_SOURCE_END -
				 EMBEDDED_SURICATTA_LUA_SOURCE_START,
			     "LuaSuricatta") ||
	     lua_pcall(gL, 0, LUA_MULTRET, 0)) != LUA_OK) {
		INFO("No compiled-in Suricatta Lua module(s) found.");
		TRACE("Lua exception:\n%s", lua_tostring(gL, -1));
		lua_pop(gL, 1);
		return SERVER_EINIT;
	}
	#else
	if (luaL_dostring(gL, "require (\"swupdate_suricatta\")") != LUA_OK) {
		ERROR("Error while executing require: %s", lua_tostring(gL, -1));
		WARN("No Suricatta Lua module(s) found.");
		if (luaL_dostring(gL, "return "
				      "package.path:gsub(';','\\n'):gsub('?','"
				      "swupdate_suricatta')") == 0) {
			lua_pop(gL, 1);
			TRACE("Suricatta Lua module search path:\n%s",
			      lua_tostring(gL, -1));
			lua_pop(gL, 1);
		}
		return SERVER_EINIT;
	}
	#endif

	#define MAP(x) #x,
	static const char *const function_names[] = { SURICATTA_FUNCS };
	#undef MAP
	for (int i = 0; i <= SURICATTA_FUNC_MANDATORY; i++) {
		if (func_registry[i] == SURICATTA_FUNC_NULL) {
			ERROR("Lua function for %s required but not registered.",
			      function_names[i]);
			return SERVER_EINIT;
		}
	}

	/* Assign __index metamethod to nil for convenient nil-indexing tables. */
	lua_pushnil(gL);
	lua_createtable(gL, 0, 1);
	lua_pushcfunction(gL, on_nil_table_index);
	lua_setfield(gL, -2, "__index");
	lua_setmetatable(gL, -2);
	lua_pop(gL, 1);

	return SERVER_OK;
}


/**
 * @brief Iterator over SWUpdate configuration section key-value pairs.
 *
 * @param  settings  Pointer to configuration section.
 * @param  data      The Lua state to pass through to callback.
 * @return 0
 */
static int config_section_to_table(void *setting, void *data)
{
	for (int i = 0; i < config_setting_length(setting); i++) {
		config_setting_t *entry = config_setting_get_elem(setting, i);
		switch (config_setting_type(entry)) {
		case CONFIG_TYPE_INT:
			push_to_table((lua_State *)data, entry->name,
				      config_setting_get_int(entry));
			break;
		case CONFIG_TYPE_INT64:
			push_to_table((lua_State *)data, entry->name,
				      config_setting_get_int64(entry));
			break;
		case CONFIG_TYPE_STRING:
			push_to_table((lua_State *)data, entry->name,
				      config_setting_get_string(entry));
			break;
		case CONFIG_TYPE_BOOL:
			push_to_table((lua_State *)data, entry->name,
				      (bool)config_setting_get_bool(entry));
			break;
		case CONFIG_TYPE_FLOAT:
			push_to_table((lua_State *)data, entry->name,
				      config_setting_get_float(entry));
			break;
		}
	}

	return 0;
}


/**
 * @brief Start the Suricatta Lua module.
 *
 * @param  fname  The configuration file to read additional configuration from.
 * @param  argc   The number of arguments.
 * @param  argv   The array of arguments.
 * @return SERVER_OK, or, in case of errors, SERVER_EINIT or SERVER_EERR.
 */
static server_op_res_t server_start(const char *fname, int argc, char *argv[])
{
	if (suricatta_lua_create() != SERVER_OK) {
		suricatta_lua_destroy();
		return SERVER_EINIT;
	}

	if (channel_curl_init() != CHANNEL_OK) {
		suricatta_lua_destroy();
		return SERVER_EINIT;
	}

	/* Prepare argument Table #1: Channel default options. */
	channel_push_options(gL, &channel_data_defaults);

	/* Prepare argument Table #2: Command line options. */
	lua_newtable(gL);
	for (char **pargv = argv + 1; *pargv != argv[argc]; pargv++) {
		lua_pushstring(gL, *pargv);
		lua_rawseti(gL, -2, (int)lua_rawlen(gL, -2) + 1);
	}

	/* Prepare argument Table #3: Configuration file options. */
	lua_newtable(gL);
	push_to_table(gL, "polldelay", CHANNEL_DEFAULT_POLLING_INTERVAL);
	if (fname) {
		swupdate_cfg_handle handle;
		swupdate_cfg_init(&handle);
		if (swupdate_cfg_read_file(&handle, fname) == 0) {
			if (read_module_settings(&handle, CONFIG_SECTION,
						 config_section_to_table, gL) != 0) {
				ERROR("Error reading module settings \"%s\" from %s",
				      CONFIG_SECTION, fname);
			}
		}
		swupdate_cfg_destroy(&handle);
	}
	return map_lua_result(call_lua_func(gL, SURICATTA_FUNC_SERVER_START, 3));
}


/**
 * @brief Stop the Suricatta Lua module.
 *
 * @return SERVER_OK or, in case of errors, any other from server_op_res_t.
 */
static server_op_res_t server_stop(void)
{
	server_op_res_t result = map_lua_result(
	    call_lua_func(gL, SURICATTA_FUNC_SERVER_STOP, 0));
	suricatta_lua_destroy();
	return result;
}


/**
 * @brief Print the Suricatta Lua module's help text.
 */
static void server_print_help(void)
{
	if (suricatta_lua_create() != SERVER_OK) {
		fprintf(stderr, "Error loading Suricatta Lua module.\n");
		suricatta_lua_destroy();
		return;
	}
	channel_push_options(gL, &channel_data_defaults);
	push_to_table(gL, "polldelay", CHANNEL_DEFAULT_POLLING_INTERVAL);
	(void)call_lua_func(gL, SURICATTA_FUNC_PRINT_HELP, 1);
	suricatta_lua_destroy();
}


/**
 * @brief Get the polling interval.
 *
 * Note: The polling interval may be fetched from the remote server within
 * the Lua realm, set locally via IPC, or be calculated otherwise by the
 * Lua function implementing server_get_polling_interval().
 *
 * @return Polling interval in seconds.
 */
static unsigned int server_get_polling_interval(void)
{
	int result = call_lua_func(gL, SURICATTA_FUNC_GET_POLLING_INTERVAL, 0);
	return result >= 0 ? (unsigned int)result : CHANNEL_DEFAULT_POLLING_INTERVAL;
}


/**
 * @brief Query the remote server for pending actions.
 *
 * @param  action_id  Action ID of the pending action.
 * @return SERVER_UPDATE_AVAILABLE and SERVER_ID_REQUESTED are handled
 *         in suricatta/suricatta.c, the others result in suricatta
 *         sleeping again.
 */
static server_op_res_t server_has_pending_action(int *action_id)
{
	lua_pushinteger(gL, *action_id);
	server_op_res_t result = map_lua_result(
	    call_lua_func(gL, SURICATTA_FUNC_HAS_PENDING_ACTION, 1));
	if ((lua_gettop(gL) > 0) && (lua_isnumber(gL, -1))) {
		/*
		 * The Lua function may return action_id only in the non-error case,
		 * so don't strictly require it to be returned.
		 */
		*action_id = (int)lua_tointeger(gL, -1);
		lua_pop(gL, 1);
	}
	return result;
}


/**
 * @brief Install an update.
 *
 * @return SERVER_OK or, in case of errors, any other from server_op_res_t.
 */
static server_op_res_t server_install_update(void)
{
	return map_lua_result(call_lua_func(gL, SURICATTA_FUNC_INSTALL_UPDATE, 0));
}


/**
 * @brief Send device configuration/data to remote server.
 *
 * @return SERVER_OK or, in case of errors, any other from server_op_res_t.
 */
static server_op_res_t server_send_target_data(void)
{
	return map_lua_result(call_lua_func(gL, SURICATTA_FUNC_SEND_TARGET_DATA, 0));
}


/**
 * @brief Handle IPC message sent to Suricatta Lua module.
 *
 * @param  msg  IPC message.
 * @return SERVER_OK or, in case of errors, any other from server_op_res_t.
 */
static server_op_res_t server_ipc(ipc_message *msg)
{
	lua_newtable(gL);
	push_to_table(gL, "magic",      msg->magic);
	push_to_table(gL, "cmd",        msg->data.procmsg.cmd);
	lua_pushstring(gL, "commands");
	lua_newtable(gL);
	push_to_table(gL, "CONFIG",     CMD_CONFIG);
	push_to_table(gL, "ACTIVATION", CMD_ACTIVATION);
	push_to_table(gL, "GET_STATUS", CMD_GET_STATUS);
	/* CMD_ENABLE is handled directly in suricatta/suricatta.c */
	lua_settable(gL, -3);
	lua_pushstring(gL, "msg");
	lua_pushlstring(gL, (char *)msg->data.procmsg.buf, msg->data.procmsg.len);
	lua_settable(gL, -3);
	if (msg->data.procmsg.len > 0) {
		lua_pushstring(gL, "json");
		struct json_object *json_root = json_tokener_parse(msg->data.procmsg.buf);
		if (!json_root || !json_to_table(gL, json_root)) {
			ERROR("Error parsing JSON IPC string: %s\n", msg->data.procmsg.buf);
			lua_pushnil(gL);
		}
		lua_settable(gL, -3);
		if (json_root && json_object_put(json_root) != 1) {
			ERROR("JSON object should be freed but was not.");
		}
	}
	server_op_res_t result = map_lua_result(call_lua_func(gL, SURICATTA_FUNC_IPC, 1));
	msg->type = result == SERVER_OK ? ACK : NACK;
	msg->data.procmsg.len = 0;
	if (lua_isstring(gL, -1)) {
		/*
		 * Dancing here to not fail on alignment-sensitive architectures as
		 * *size_t has a different alignment requirement than *unsigned int.
		 */
		size_t len;
		(void)strncpy(msg->data.procmsg.buf, lua_tolstring(gL, -1, &len),
			      sizeof(msg->data.procmsg.buf) - 1);
		msg->data.procmsg.buf[sizeof(msg->data.procmsg.buf)-1] = '\0';
		msg->data.procmsg.len = (unsigned int)len;
		lua_pop(gL, 1);
	}
	return result;
}

static server_t server = {
	.has_pending_action = &server_has_pending_action,
	.install_update = &server_install_update,
	.send_target_data = &server_send_target_data,
	.get_polling_interval = &server_get_polling_interval,
	.start = &server_start,
	.stop = &server_stop,
	.ipc = &server_ipc,
	.help = &server_print_help,
};

__attribute__((constructor))
static void register_server_lua(void)
{
	register_server("lua", &server);
}
