/*
 * (C) Copyright 2017
 * Stefano Babic, stefano.babic@swupdate.org.
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

#include <libuboot.h>
#ifndef CONFIG_UBOOT_DEFAULTENV
#define CONFIG_UBOOT_DEFAULTENV	"/etc/u-boot-initial-env"
#endif

static struct {
	int   (*open)(struct uboot_ctx*);
	void  (*close)(struct uboot_ctx *ctx);
	void  (*exit)(struct uboot_ctx *ctx);
	int   (*initialize)(struct uboot_ctx **out, struct uboot_env_device *envdevs);
	char* (*get_env)(struct uboot_ctx *ctx, const char *varname);
	int   (*read_config)(struct uboot_ctx *ctx, const char *config);
	int   (*load_file)(struct uboot_ctx *ctx, const char *filename);
	int   (*set_env)(struct uboot_ctx *ctx, const char *varname, const char *value);
	int   (*env_store)(struct uboot_ctx *ctx);
} libuboot;

static int bootloader_initialize(struct uboot_ctx **ctx)
{
	int ret;
	const char *namespace = NULL;

	ret = libuboot_read_config_ext(ctx, get_fwenv_config());
	if (ret) {
		ERROR("Cannot initialize environment from %s", get_fwenv_config());
		return -EINVAL;
	}

	namespace = libuboot_namespace_from_dt();

	if (namespace)
		*ctx = libuboot_get_namespace(*ctx, namespace);

	if (libuboot.open(*ctx) < 0) {
		WARN("Cannot read environment, using default");
		if (libuboot.load_file(*ctx, CONFIG_UBOOT_DEFAULTENV) < 0) {
			ERROR("Error: Cannot read default environment from file");
			return -ENODATA;
		}
	}

	return 0;
}

static int do_env_set(const char *name, const char *value)
{
	int ret;
	struct uboot_ctx *ctx = NULL;

	ret = bootloader_initialize(&ctx);
	if (!ret) {
		libuboot.set_env(ctx, name, value);
		ret = libuboot.env_store(ctx);
	}

	libuboot.close(ctx);
	libuboot.exit(ctx);

	return ret;
}

static int do_env_unset(const char *name)
{
	return do_env_set(name, NULL);
}

static int do_apply_list(const char *filename)
{
	int ret;
	struct uboot_ctx *ctx = NULL;

	ret = bootloader_initialize(&ctx);
	if (!ret) {
		libuboot.load_file(ctx, filename);
		ret = libuboot.env_store(ctx);
	}

	libuboot.close(ctx);
	libuboot.exit(ctx);

	return ret;
}

static char *do_env_get(const char *name)
{
	int ret;
	struct uboot_ctx *ctx = NULL;
	char *value = NULL;

	ret = bootloader_initialize(&ctx);
	if (!ret) {
		value = libuboot.get_env(ctx, name);
	}
	libuboot.close(ctx);
	libuboot.exit(ctx);

	return value;
}

static bootloader uboot = {
	.env_get = &do_env_get,
	.env_set = &do_env_set,
	.env_unset = &do_env_unset,
	.apply_list = &do_apply_list
};

/*
 * libubootenv is not only used as interface to U-Boot.
 * It is also used to save SWUpdate's persistent variables that
 * survives after a restart of the device but should not be
 * considered by the bootloader. That requires libubootenv
 * is always linked.
 */
static bootloader* probe(void)
{
	libuboot.open = libuboot_open;
	libuboot.close = libuboot_close;
	libuboot.exit = libuboot_exit;
	libuboot.initialize = libuboot_initialize;
	libuboot.get_env = libuboot_get_env;
	libuboot.read_config = libuboot_read_config;
	libuboot.load_file = libuboot_load_file;
	libuboot.set_env = libuboot_set_env;
	libuboot.env_store = libuboot_env_store;
	return &uboot;
}

__attribute__((constructor))
static void uboot_probe(void)
{
	(void)register_bootloader(BOOTLOADER_UBOOT, probe());
}
