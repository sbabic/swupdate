/*
 * (C) Copyright 2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
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
	if (libuboot.initialize(ctx, NULL) < 0) {
		ERROR("Error: environment not initialized");
		return -ENODEV;
	}
	if (libuboot.read_config(*ctx, CONFIG_UBOOT_FWENV) < 0) {
		ERROR("Configuration file %s wrong or corrupted", CONFIG_UBOOT_FWENV);
		return -EINVAL;
	}
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

static bootloader* probe(void)
{
#if defined(BOOTLOADER_STATIC_LINKED)
	libuboot.open = libuboot_open;
	libuboot.close = libuboot_close;
	libuboot.exit = libuboot_exit;
	libuboot.initialize = libuboot_initialize;
	libuboot.get_env = libuboot_get_env;
	libuboot.read_config = libuboot_read_config;
	libuboot.load_file = libuboot_load_file;
	libuboot.set_env = libuboot_set_env;
	libuboot.env_store = libuboot_env_store;
#else
	void* handle = dlopen("libubootenv.so.0", RTLD_NOW | RTLD_GLOBAL);
	if (!handle) {
		return NULL;
	}

	(void)dlerror();
	load_symbol(handle, &libuboot.open, "libuboot_open");
	load_symbol(handle, &libuboot.close, "libuboot_close");
	load_symbol(handle, &libuboot.exit, "libuboot_exit");
	load_symbol(handle, &libuboot.initialize, "libuboot_initialize");
	load_symbol(handle, &libuboot.get_env, "libuboot_get_env");
	load_symbol(handle, &libuboot.read_config, "libuboot_read_config");
	load_symbol(handle, &libuboot.load_file, "libuboot_load_file");
	load_symbol(handle, &libuboot.set_env, "libuboot_set_env");
	load_symbol(handle, &libuboot.env_store, "libuboot_env_store");
#endif
	return &uboot;
}

__attribute__((constructor))
static void uboot_probe(void)
{
	(void)register_bootloader(BOOTLOADER_UBOOT, probe());
}
