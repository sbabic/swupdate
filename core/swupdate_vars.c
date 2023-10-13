/*
 * (C) Copyright 2023
 * Stefano Babic, <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <dirent.h>
#include "generated/autoconf.h"
#include "util.h"
#include "pctl.h"
#include <network_ipc.h>

#include "swupdate_vars.h"

char *namespace_default = NULL;

static inline void libuboot_cleanup(struct uboot_ctx *ctx)
{
	libuboot_close(ctx);
	libuboot_exit(ctx);
}

int swupdate_vars_initialize(struct uboot_ctx **ctx, const char *namespace)
{
	int ret;

	if (!namespace || !strlen(namespace)) {
		if(!namespace_default)
			return -EINVAL;
		namespace = namespace_default;
	}

	ret = libuboot_read_config_ext(ctx, get_fwenv_config());
	if (ret) {
		ERROR("Cannot initialize environment from %s", get_fwenv_config());
		return -EINVAL;
	}

	*ctx = libuboot_get_namespace(*ctx, namespace);

	if (libuboot_open(*ctx) < 0) {
		WARN("Cannot read environment, maybe still empty ?");
	}

	return 0;
}

static char *__swupdate_vars_get(const char *name, const char *namespace)
{
	int ret;
	struct uboot_ctx *ctx = NULL;
	char *value = NULL;

	ret = swupdate_vars_initialize(&ctx, namespace);
	if (!ret) {
		value = libuboot_get_env(ctx, name);
	}
	libuboot_cleanup(ctx);

	return value;
}

char *swupdate_vars_get(const char *name, const char *namespace)
{
	if (!name)
		return NULL;

	if (pid == getpid())
	{
		ipc_message msg;
		memset(&msg, 0, sizeof(msg));

		msg.type = GET_SWUPDATE_VARS;
		if (namespace)
			strlcpy(msg.data.vars.varnamespace, namespace, sizeof(msg.data.vars.varnamespace));
		strlcpy(msg.data.vars.varname, name, sizeof(msg.data.vars.varname));

		if (ipc_send_cmd(&msg) || msg.type == NACK) {
			ERROR("Failed to get variable %s", name);
			return NULL;
		}
		return strdup (msg.data.vars.varvalue);
	}

	return __swupdate_vars_get(name, namespace);
}

static int __swupdate_vars_set(const char *name, const char *value, const char *namespace)
{
	int ret;
	struct uboot_ctx *ctx = NULL;

	ret = swupdate_vars_initialize(&ctx, namespace);
	if (!ret) {
		libuboot_set_env(ctx, name, value);
		ret = libuboot_env_store(ctx);
	}

	libuboot_cleanup(ctx);

	return ret;
}

int swupdate_vars_set(const char *name, const char *value, const char *namespace)
{
	ipc_message msg;

	if (!name)
		return -EINVAL;

	if (pid == getpid()) {
		memset(&msg, 0, sizeof(msg));
		msg.magic = IPC_MAGIC;
		msg.type = SET_SWUPDATE_VARS;
		if (namespace)
			strlcpy(msg.data.vars.varnamespace, namespace, sizeof(msg.data.vars.varnamespace));
		strlcpy(msg.data.vars.varname, name, sizeof(msg.data.vars.varname));
		if (value)
			strlcpy(msg.data.vars.varvalue, value, sizeof(msg.data.vars.varvalue));
		return !(ipc_send_cmd(&msg) == 0 && msg.type == ACK);
	}

	return __swupdate_vars_set(name, value, namespace);
}

int swupdate_vars_unset(const char *name, const char *namespace)
{
	return swupdate_vars_set(name, NULL, namespace);
}

int swupdate_vars_apply_list(const char *filename, const char *namespace)
{
	int ret;
	struct uboot_ctx *ctx = NULL;

	if (pid == getpid()) {
		ERROR("This function can be called only by core !");
		return -EINVAL;
	}
	ret = swupdate_vars_initialize(&ctx, namespace);
	if (!ret) {
		libuboot_load_file(ctx, filename);
		ret = libuboot_env_store(ctx);
	}

	libuboot_cleanup(ctx);

	return ret;
}

bool swupdate_set_default_namespace(const char *namespace)
{
	if (namespace_default)
		free(namespace_default);

	namespace_default = strdup(namespace);

	return namespace_default != NULL;
}
