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
#include "bootloader.h"

#include <libuboot.h>
#ifndef CONFIG_UBOOT_DEFAULTENV
#define CONFIG_UBOOT_DEFAULTENV	"/etc/u-boot-initial-env"
#endif

static int bootloader_initialize(struct uboot_ctx **ctx)
{
	if (libuboot_initialize(ctx, NULL) < 0) {
		ERROR("Error: environment not initialized");
		return -ENODEV;
	}
	if (libuboot_read_config(*ctx, CONFIG_UBOOT_FWENV) < 0) {
		ERROR("Configuration file %s wrong or corrupted", CONFIG_UBOOT_FWENV);
		return -EINVAL;
	}
	if (libuboot_open(*ctx) < 0) {
		WARN("Cannot read environment, using default");
		if (libuboot_load_file(*ctx, CONFIG_UBOOT_DEFAULTENV) < 0) {
			ERROR("Error: Cannot read default environment from file");
			return -ENODATA;
		}
	}

	return 0;
}

int bootloader_env_set(const char *name, const char *value)
{
	int ret;
	struct uboot_ctx *ctx = NULL;

	ret = bootloader_initialize(&ctx);
	if (!ret) {
		libuboot_set_env(ctx, name, value);
		ret = libuboot_env_store(ctx);
	}

	libuboot_close(ctx);
	libuboot_exit(ctx);

	return ret;
}

int bootloader_env_unset(const char *name)
{
	return bootloader_env_set(name, NULL);
}


int bootloader_apply_list(const char *filename)
{
	int ret;
	struct uboot_ctx *ctx = NULL;

	ret = bootloader_initialize(&ctx);
	if (!ret) {
		libuboot_load_file(ctx, filename);
		ret = libuboot_env_store(ctx);
	}

	libuboot_close(ctx);
	libuboot_exit(ctx);

	return ret;
}

char *bootloader_env_get(const char *name)
{
	int ret;
	struct uboot_ctx *ctx = NULL;
	char *value = NULL;

	ret = bootloader_initialize(&ctx);
	if (!ret) {
		value = libuboot_get_env(ctx, name);
	}
	libuboot_close(ctx);
	libuboot_exit(ctx);

	return value;
}
