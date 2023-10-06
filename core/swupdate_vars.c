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
#include "dlfcn.h"
#include "bootloader.h"

#include "swupdate_vars.h"

static inline void libuboot_cleanup(struct uboot_ctx *ctx)
{
	libuboot_close(ctx);
	libuboot_exit(ctx);
}

int swupdate_vars_initialize(struct uboot_ctx **ctx, const char *namespace)
{
	int ret;

	if (!namespace)
		return -EINVAL;

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

char *swupdate_vars_get(const char *name, const char *namespace)
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

int swupdate_vars_set(const char *name, const char *value, const char *namespace)
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

int swupdate_vars_unset(const char *name, const char *namespace)
{
	return swupdate_vars_set(name, NULL, namespace);
}

int swupdate_vars_apply_list(const char *filename, const char *namespace)
{
	int ret;
	struct uboot_ctx *ctx = NULL;

	ret = swupdate_vars_initialize(&ctx, namespace);
	if (!ret) {
		libuboot_load_file(ctx, filename);
		ret = libuboot_env_store(ctx);
	}

	libuboot_cleanup(ctx);

	return ret;
}
