/*
 * (C) Copyright 2018
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
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

static int progress_connect(lua_State *L) {
	struct prog_obj *p = (struct prog_obj *) auxiliar_checkclass(L, "swupdate_progress", 1);
	int connfd;

	close(p->socket);
	connfd = progress_ipc_connect(WAIT);
	if (connfd < 0) {
		lua_pushnil(L);
		return 2;
	}
	p->socket = connfd;
	p->status = IDLE;
	return 1;
}

static int progress_close(lua_State __attribute__ ((__unused__)) *L) {
	return 1;
}

static int progress_receive(lua_State *L) {
	struct prog_obj *p = (struct prog_obj *) auxiliar_checkclass(L, "swupdate_progress", 1);
	int connfd = p->socket;
	if (progress_ipc_receive(&connfd, &p->msg) == -1) {
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

	int connfd;

	connfd = progress_ipc_connect(WAIT);

	if (connfd < 0) {
		lua_pushnil(L);
		return 2;
	}

	/* allocate   progress object */
	struct prog_obj *p = (struct prog_obj *) lua_newuserdata(L, sizeof(*p));
	p->socket = connfd;
	p->status = IDLE;

	/* set its type as master object */
	auxiliar_setclass(L, "swupdate_progress", -1);

	return 1;

}
 
static const luaL_Reg lua_swupdate[] = {
  {"progress", progress},
  {"ipv4", netif},
  {NULL, NULL}
};

/*
 * Initialization of C module
 */
int luaopen_lua_swupdate(lua_State *L){
	luaL_newlib(L, lua_swupdate);
	auxiliar_newclass(L, "swupdate_progress", progress_methods);
	return 1;
}
