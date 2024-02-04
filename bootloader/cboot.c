/*
 * (C) Copyright 2023 Stefano Babic, stefano.babic@swupdate.org
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * This uses the API of the libtegra-boot-tools library
 * see info on https://github.com/OE4T/tegra-boot-tools
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include "generated/autoconf.h"
#include "util.h"
#include "dlfcn.h"
#include "bootloader.h"

/*
 * thes edefines are not exported by the library
 */
#define BOOTINFO_O_RDONLY (0<<0)
#define BOOTINFO_O_RDWR	  (3<<0)
#define BOOTINFO_O_CREAT  (1<<2)
#define BOOTINFO_O_FORCE_INIT (1<<3)

/*
 * Define the API from bootinfo.h. File is not (yet ?)
 * exported by the library, and they are defined here.
 */
struct bootinfo_context_s;
typedef struct bootinfo_context_s bootinfo_context_t;
extern int bootinfo_open(unsigned int flags, bootinfo_context_t **ctxp);
extern int bootinfo_close(bootinfo_context_t *ctx);
extern int bootinfo_var_get(bootinfo_context_t *ctx, const char *name, char *valuebuf, size_t valuebuf_size);
extern int bootinfo_var_set(bootinfo_context_t *ctx, const char *name, const char *value);

/*
 * Structure for external library callbacks
 */
static struct {
	int (*open)(unsigned int flags, bootinfo_context_t **ctxp);
	int (*close)(bootinfo_context_t *ctx);
	int (*get_env)(bootinfo_context_t *ctx, const char *name, char *valuebuf, size_t valuebuf_size);
	int (*set_env)(bootinfo_context_t *ctx, const char *name, const char *value);
} libcboot;

static char *do_env_get(const char *name)
{
	bootinfo_context_t *ctx;
	static char valuebuf[65536];
	char *value;

	if (!name)
		return NULL;

	if (libcboot.open(BOOTINFO_O_RDONLY, &ctx) < 0) {
		ERROR("libcboot.open returns with error");
		return NULL;
	}

	if (libcboot.get_env(ctx, name, valuebuf, sizeof(valuebuf)) < 0)
		valuebuf[0] = '\0';

	value = strdup(valuebuf);

	if (libcboot.close(ctx) < 0) {
		ERROR("libcboot.close returns with error, environmen not saved");
	}
	return value;
}

static int do_env_set(const char *name, const char *value)
{
	int ret = 0;
	bootinfo_context_t *ctx;

	if (!name || !value)
		return -EINVAL;

	if (libcboot.open(BOOTINFO_O_RDWR, &ctx) < 0) {
		ERROR("libcboot.open returns with error");
		return -ENOENT;
	}
	if (libcboot.set_env(ctx, name, value) < 0) {
		ERROR("libcboot.set_env");
		ret = -EFAULT;
	}
	if (libcboot.close(ctx) < 0) {
		ERROR("libcboot.close returns with error, environment not saved");
		ret = -EFAULT;
	}
	return ret;
}

static int do_env_unset(const char *name)
{
	int ret = 0;
	bootinfo_context_t *ctx;

	if (!name)
		return -EINVAL;

	if (libcboot.open(BOOTINFO_O_RDWR, &ctx) < 0) {
		ERROR("libcboot.open returns with error");
		return -ENOENT;
	}
	if (libcboot.set_env(ctx, name, NULL) < 0) {
		ERROR("libcboot.set_env for unset");
		ret = -EFAULT;
	}
	if (libcboot.close(ctx) < 0) {
		ERROR("libcboot.close returns with error, environment not saved");
		ret = -EFAULT;
	}
	return ret;
}

static int do_apply_list(const char *filename)
{
	errno = 0;
	bootinfo_context_t *ctx;

	FILE *file = fopen(filename, "rb");
	if (!file) {
		ERROR("Cannot open bootloader environment source file %s: %s",
		      filename, strerror(errno));
		return -EIO;
	}

	char *line = NULL;
	size_t length = 0;
	int result = 0;
	if (libcboot.open(BOOTINFO_O_RDWR, &ctx) < 0) {
		ERROR("libcboot.open returns with error");
		close(file);
		return -ENOENT;
	}
	while ((getline(&line, &length, file)) != -1) {
		char *key = strtok(line, "=");
		char *value = strtok(NULL, "\t\n");
		if (key != NULL) {
			result = libcboot.set_env(ctx, key, value);
			if (result < 0) {
				ERROR("Error %s boot var %s(%s)", value ? "storing" : "deleting",
					key, value ? value : "");
			}
		}
	}

	if (libcboot.close(ctx) < 0) {
		ERROR("libcboot.close returns with error, environment not saved");
		result = -EFAULT;
	}
	fclose(file);
	free(line);
	return result;
}

static bootloader cboot = {
	.env_get = &do_env_get,
	.env_set = &do_env_set,
	.env_unset = &do_env_unset,
	.apply_list = &do_apply_list
};

static bootloader* probe(void)
{
#if defined(BOOTLOADER_STATIC_LINKED)
	libcboot.open = bootinfo_open;
	libcboot.close = bootinfo_close;
	libcboot.get_env = bootinfo_var_get;
	libcboot.set_env = bootinfo_var_set;
#else
	void* handle = dlopen("libtegra-boot-tools.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!handle) {
		return NULL;
	}

	(void)dlerror();
	load_symbol(handle, &libcboot.open, "bootinfo_open");
	load_symbol(handle, &libcboot.close, "bootinfo_close");
	load_symbol(handle, &libcboot.get_env, "bootinfo_var_get");
	load_symbol(handle, &libcboot.set_env, "bootinfo_var_set");
#endif
	return &cboot;
}

__attribute__((constructor))
static void cboot_probe(void)
{
	(void)register_bootloader(BOOTLOADER_CBOOT, probe());
}
