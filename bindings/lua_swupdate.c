/*
 * (C) Copyright 2018
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#include <progress_ipc.h>
#include <lua.h>
#include <lua_util.h>
#include <lauxlib.h>
#include <string.h> 
#include <ifaddrs.h>
#include <netinet/in.h> 
#include <net/if.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/select.h>
#include <network_ipc.h>
#include <stdlib.h>

#include "auxiliar.h"

#define WAIT 1

#define LUA_PUSH_STRING(key, data)  do { 	\
	lua_pushstring(L, key);			\
	lua_pushstring(L, data);		\
	lua_settable(L, -3);			\
} while (0)

#define LUA_PUSH_BOOL(key, data)  do { 		\
	lua_pushstring(L, key);			\
	lua_pushboolean(L, data);		\
	lua_settable(L, -3);			\
} while (0)

#define LUA_PUSH_NUMBER(key, data)  do { 	\
	lua_pushstring(L, key);			\
	lua_pushnumber(L, (double) data);	\
	lua_settable(L, -3);			\
} while (0)

#define LUA_PUSH_INT(key, data)  do { 		\
	lua_pushstring(L, key);			\
	lua_pushinteger(L, data);		\
	lua_settable(L, -3);			\
} while (0)

static int progress(lua_State *L);
static int progress_connect(lua_State *L);
static int progress_receive(lua_State *L);
static int progress_close(lua_State *L);

static int ctrl(lua_State *L);
static int ctrl_connect(lua_State *L);
static int ctrl_write(lua_State *L);
static int ctrl_close(lua_State *L);
static int ctrl_close_socket(lua_State *L);

int luaopen_lua_swupdate(lua_State *L);

/*
 * Return a table with all interface and their IP address
 */
static int netif(lua_State *L)
{

	struct ifaddrs * ifAddrStruct=NULL;
	struct ifaddrs * ifa=NULL;
	void *tmpAddrPtr=NULL;

	getifaddrs(&ifAddrStruct);

	lua_newtable(L);

	for (ifa = ifAddrStruct; ifa != NULL; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr) {
			continue;
		}
		/* Check if running */
		if ((ifa->ifa_flags & IFF_UP) == 0)
			continue;

		if ((ifa->ifa_flags & IFF_LOOPBACK) != 0)
			continue;

		if (ifa->ifa_addr->sa_family == AF_INET) { // Should be IPv4
			char addressBuffer[INET_ADDRSTRLEN];
			char netmaskBuffer[INET_ADDRSTRLEN];
			char buf[2 * INET_ADDRSTRLEN];
			tmpAddrPtr=&((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
			inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
			tmpAddrPtr=&((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr;
			inet_ntop(AF_INET, tmpAddrPtr, netmaskBuffer, INET_ADDRSTRLEN);
			snprintf(buf, sizeof(buf), "%s %s", addressBuffer, netmaskBuffer);
			LUA_PUSH_STRING(ifa->ifa_name, buf);
		}
	}

	if (ifAddrStruct!=NULL) freeifaddrs(ifAddrStruct);

	return 1;
}

struct ctrl_obj {
	int socket;
};

/* control object methods */
static luaL_Reg ctrl_methods[] = {
	{"__gc",       ctrl_close_socket},
	{"__tostring", auxiliar_tostring},
	{"connect",    ctrl_connect},
	{"write",      ctrl_write},
	{"close",      ctrl_close},
	{NULL,         NULL}
};

/**
 * @brief Connect to SWUpdate control socket.
 *
 * @param  [Lua] The swupdate_control class instance.
 * @return [Lua] The connection handle (mostly for information), or,
 *               in case of errors, nil plus an error message.
 */
static int ctrl_connect(lua_State *L) {
	struct ctrl_obj *p = (struct ctrl_obj *) auxiliar_checkclass(L, "swupdate_control", 1);
	struct swupdate_request req;
	if (p->socket != -1) {
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushstring(L, "Already connected to SWUpdate control socket.");
		return 2;
	}

	swupdate_prepare_req(&req);
	req.source = SOURCE_LOCAL;
	int connfd = ipc_inst_start_ext(&req, sizeof(req));
	if (connfd < 0) {
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushstring(L, "Cannot connect to SWUpdate control socket.");
		return 2;
	}

	p->socket = connfd;

	lua_pop(L, 1);
	lua_pushnumber(L, connfd);
	lua_pushnil(L);

	return 2;
}

/**
 * @brief Write data chunk to SWUpdate's control socket.
 *
 * @param  [Lua] The swupdate_control class instance.
 * @param  [Lua] Lua String chunk data to write to SWUpdate's control socket.
 * @return [Lua] True, or, in case of errors, nil plus an error message.
 */
static int ctrl_write(lua_State *L) {
	struct ctrl_obj *p = (struct ctrl_obj *) auxiliar_checkclass(L, "swupdate_control", 1);
	luaL_checktype(L, 2, LUA_TSTRING);

	if (p->socket == -1) {
		lua_pushnil(L);
		lua_pushstring(L, "Not connected to SWUpdate control socket.");
		goto ctrl_write_exit;
	}

	size_t len = 0;
	const char* buf = lua_tolstring(L, 2, &len);
	if (!buf) {
		lua_pushnil(L);
		lua_pushstring(L, "Error converting Lua chunk data.");
		goto ctrl_write_exit;
	}
	if (ipc_send_data(p->socket, (char *)buf, len) < 0) {
		lua_pushnil(L);
		lua_pushstring(L, "Error writing to SWUpdate control socket.");
		goto ctrl_write_exit;
	}

	lua_pushboolean(L, true);
	lua_pushnil(L);

ctrl_write_exit:
	lua_remove(L, 1);
	lua_remove(L, 1);
	return 2;
}

static int ctrl_close_socket(lua_State *L) {
	struct ctrl_obj *p = (struct ctrl_obj *) auxiliar_checkclass(L, "swupdate_control", 1);
	(void)ipc_end(p->socket);
	p->socket = -1;
	lua_remove(L, 1);
	return 0;
}

static char *ipc_wait_error_msg = NULL;
static int ipc_wait_get_msg(ipc_message *msg)
{
	if (msg->data.status.error != 0 && msg->data.status.current == FAILURE) {
		free(ipc_wait_error_msg);
		ipc_wait_error_msg = strdup(msg->data.status.desc);
	}
	return 0;
}

/**
 * @brief Close connection to SWUpdate control socket.
 *
 * @param  [Lua] The swupdate_control class instance.
 * @return [Lua] True, or, in case of errors, nil plus an error message.
 */
static int ctrl_close(lua_State *L) {
	struct ctrl_obj *p = (struct ctrl_obj *) auxiliar_checkclass(L, "swupdate_control", 1);
	if (p->socket == -1) {
		lua_pop(L, 1);
		lua_pushboolean(L, true);
		lua_pushnil(L);
		return 2;
	}

	(void)ctrl_close_socket(L);

	if ((RECOVERY_STATUS)ipc_wait_for_complete(ipc_wait_get_msg) == FAILURE) {
		lua_pushnil(L);
		lua_pushstring(L, ipc_wait_error_msg);
		free(ipc_wait_error_msg);
		ipc_wait_error_msg = NULL;
		return 2;
	}

	ipc_message msg;
	msg.data.procmsg.len = 0;
	if (ipc_postupdate(&msg) != 0 || msg.type != ACK) {
		lua_pushnil(L);
		lua_pushstring(L, "SWUpdate succeeded but post-update action failed.");
		return 2;
	}

	lua_pushboolean(L, true);
	lua_pushnil(L);
	return 2;
}

static int ctrl(lua_State *L) {
	/* allocate control object */
	struct ctrl_obj *p = (struct ctrl_obj *) lua_newuserdata(L, sizeof(*p));
	p->socket = -1;

	/* set its type as master object */
	auxiliar_setclass(L, "swupdate_control", -1);

	return 1;
}

struct prog_obj {
	RECOVERY_STATUS	status;
	int	socket;
	struct	progress_msg msg;
};

/* progress object methods */
static luaL_Reg progress_methods[] = {
    {"__gc",        progress_close},
    {"__tostring",  auxiliar_tostring},
    {"close",       progress_close},
    {"connect",     progress_connect},
    {"receive",     progress_receive},
    {NULL,          NULL}
};

/**
 * @brief Connect to SWUpdate progress socket.
 *
 * @param  [Lua] The swupdate_progress class instance.
   @return [Lua] The connection handle (mostly for information), or,
 *               in case of errors, nil plus an error message.
 */
static int progress_connect(lua_State *L) {
	struct prog_obj *p = (struct prog_obj *) auxiliar_checkclass(L, "swupdate_progress", 1);
	int connfd;

	close(p->socket);
	connfd = progress_ipc_connect(WAIT);
	if (connfd < 0) {
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushstring(L, "Cannot connect to SWUpdate progress socket.");
		return 2;
	}
	p->socket = connfd;
	p->status = IDLE;

	lua_pop(L, 1);
	lua_pushnumber(L, connfd);
	lua_pushnil(L);

	return 2;
}

static int progress_close(lua_State __attribute__ ((__unused__)) *L) {
	return 1;
}

static int progress_receive(lua_State *L) {
	struct prog_obj *p = (struct prog_obj *) auxiliar_checkclass(L, "swupdate_progress", 1);
	int connfd = p->socket;
	if (progress_ipc_receive(&connfd, &p->msg) <= 0) {
        	lua_pushnil(L);
		return 2;
	};
	lua_newtable(L);

	LUA_PUSH_INT("status", p->msg.status);
	LUA_PUSH_INT("download", p->msg.dwl_percent);
	LUA_PUSH_INT("source", p->msg.source);
	LUA_PUSH_INT("nsteps", p->msg.nsteps);
	LUA_PUSH_INT("step", p->msg.cur_step);
	LUA_PUSH_INT("percent", p->msg.cur_percent);
	LUA_PUSH_STRING("artifact", p->msg.cur_image);
	LUA_PUSH_STRING("handler", p->msg.hnd_name);
	if (p->msg.infolen)
		LUA_PUSH_STRING("info", p->msg.info);

	p->status = p->msg.status;

	return 1;
}

static int progress(lua_State *L) {
	/* allocate progress object */
	struct prog_obj *p = (struct prog_obj *) lua_newuserdata(L, sizeof(*p));
	p->socket = -1;
	p->status = IDLE;

	/* set its type as master object */
	auxiliar_setclass(L, "swupdate_progress", -1);

	return 1;
}

static const luaL_Reg lua_swupdate[] = {
  {"progress", progress},
  {"control", ctrl},
  {"ipv4", netif},
  {NULL, NULL}
};

/*
 * Initialization of C module
 */
int luaopen_lua_swupdate(lua_State *L){
	luaL_newlib(L, lua_swupdate);

	/* Export the RECOVERY_STATUS enum */
	lua_pushstring(L, "RECOVERY_STATUS");
	lua_newtable (L);
	LUA_PUSH_INT("IDLE", IDLE);
	LUA_PUSH_INT("START", START);
	LUA_PUSH_INT("RUN", RUN);
	LUA_PUSH_INT("SUCCESS", SUCCESS);
	LUA_PUSH_INT("FAILURE", FAILURE);
	LUA_PUSH_INT("DOWNLOAD", DOWNLOAD);
	LUA_PUSH_INT("DONE", DONE);
	LUA_PUSH_INT("SUBPROCESS", SUBPROCESS);
	LUA_PUSH_INT("PROGRESS", PROGRESS);
	lua_settable(L, -3);

	/* Export the sourcetype enum */
	lua_pushstring(L, "sourcetype");
	lua_newtable (L);
	LUA_PUSH_INT("SOURCE_UNKNOWN", SOURCE_UNKNOWN);
	LUA_PUSH_INT("SOURCE_WEBSERVER", SOURCE_WEBSERVER);
	LUA_PUSH_INT("SOURCE_SURICATTA", SOURCE_SURICATTA);
	LUA_PUSH_INT("SOURCE_DOWNLOADER", SOURCE_DOWNLOADER);
	LUA_PUSH_INT("SOURCE_LOCAL", SOURCE_LOCAL);
	LUA_PUSH_INT("SOURCE_CHUNKS_DOWNLOADER", SOURCE_CHUNKS_DOWNLOADER);
	lua_settable(L, -3);

	auxiliar_newclass(L, "swupdate_progress", progress_methods);
	auxiliar_newclass(L, "swupdate_control", ctrl_methods);
	return 1;
}
