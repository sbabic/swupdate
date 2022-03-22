/*
 * Author: Christian Storm
 * Author: Andreas Reichel
 * Copyright (C) 2018, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <util.h>
#include <efibootguard/ebgenv.h>
#include <generated/autoconf.h>
#include <state.h>
#include "dlfcn.h"
#include "bootloader.h"

static struct {
	void (*beverbose)(ebgenv_t *e, bool v);
	int  (*env_create_new)(ebgenv_t *e);
	int  (*env_open_current)(ebgenv_t *e);
	int  (*env_get)(ebgenv_t *e, char *key, char* buffer);
	int  (*env_set)(ebgenv_t *e, char *key, char *value);
	int  (*env_set_ex)(ebgenv_t *e, char *key, uint64_t datatype, uint8_t *value, uint32_t datalen);
	uint16_t (*env_getglobalstate)(ebgenv_t *e);
	int  (*env_setglobalstate)(ebgenv_t *e, uint16_t ustate);
	int  (*env_close)(ebgenv_t *e);
	int  (*env_finalize_update)(ebgenv_t *e);
} libebg;

static ebgenv_t ebgenv = {0};

static int do_env_set(const char *name, const char *value)
{
	int ret;

	errno = 0;
	libebg.beverbose(&ebgenv, loglevel > INFOLEVEL ? true : false);

	DEBUG("Setting %s=%s in bootloader environment", name, value);

	if ((ret = libebg.env_open_current(&ebgenv)) != 0) {
		ERROR("Cannot open current bootloader environment: %s.", strerror(ret));
		return ret;
	}

	if (strncmp(name, BOOTVAR_TRANSACTION, strlen(name) + 1) == 0 &&
	    strncmp(value, get_state_string(STATE_IN_PROGRESS),
		    strlen(get_state_string(STATE_IN_PROGRESS)) + 1) == 0) {
		/* Open or create a new environment to reflect
		 * EFI Boot Guard's representation of SWUpdate's
		 * recovery_status=in_progress. */
		if ((ret = libebg.env_create_new(&ebgenv)) != 0) {
			ERROR("Cannot open/create new bootloader environment: %s.",
			     strerror(ret));
		}
	} else if (strncmp(name, (char *)STATE_KEY, strlen((char *)STATE_KEY) + 1) == 0) {
		/* Map suricatta's update_state_t to EFI Boot Guard's API. */
		if ((ret = libebg.env_setglobalstate(&ebgenv, *value - '0')) != 0) {
			ERROR("Cannot set %s=%s in bootloader environment.", STATE_KEY, value);
		}
	} else {
		/* A new environment is created if EFI Boot Guard's
		 * representation of SWUpdate's recovery_status is
		 * not in_progress. */
		if ((ret = libebg.env_create_new(&ebgenv)) != 0) {
			ERROR("Cannot open/create new bootloader environment: %s.",
			     strerror(ret));
			return ret;
		}
		if ((ret = libebg.env_set(&ebgenv, (char *)name, (char *)value)) != 0) {
			ERROR("Cannot set %s=%s in bootloader environment: %s.",
			    name, value, strerror(ret));
		}
	}
	(void)libebg.env_close(&ebgenv);

	return ret;
}

static int do_env_unset(const char *name)
{
	int ret;

	libebg.beverbose(&ebgenv, loglevel > INFOLEVEL ? true : false);

	if ((ret = libebg.env_open_current(&ebgenv)) != 0) {
		ERROR("Cannot open current bootloader environment: %s.", strerror(ret));
		return ret;
	}

	if (strncmp(name, BOOTVAR_TRANSACTION, strlen(name) + 1) == 0) {
		ret = libebg.env_finalize_update(&ebgenv);
		if (ret) {
			ERROR("Cannot unset %s in bootloader environment: %s.", BOOTVAR_TRANSACTION, strerror(ret));
		}
	} else if (strncmp(name, (char *)STATE_KEY, strlen((char *)STATE_KEY) + 1) == 0) {
		/* Unsetting STATE_KEY is semantically equivalent to setting it to STATE_OK. */
		if ((ret = libebg.env_setglobalstate(&ebgenv, STATE_OK - '0')) != 0) {
			ERROR("Cannot unset %s in bootloader environment.", STATE_KEY);
		}
	} else {
		ret = libebg.env_set_ex(&ebgenv, (char *)name, USERVAR_TYPE_DELETED, (uint8_t *)"", 1);
	}
	(void)libebg.env_close(&ebgenv);

	return ret;
}

static char *do_env_get(const char *name)
{
	char *value = NULL;
	size_t size;

	errno = 0;
	libebg.beverbose(&ebgenv, loglevel > INFOLEVEL ? true : false);

	int ret;
	if ((ret = libebg.env_open_current(&ebgenv)) != 0) {
		ERROR("Cannot open current bootloader environment: %s.",
		     strerror(ret));
		return NULL;
	}

	if (strncmp(name, (char *)STATE_KEY, strlen((char *)STATE_KEY) + 1) == 0) {
		value = (char *)malloc(sizeof(char));
		*value = libebg.env_getglobalstate(&ebgenv);
	} else {
		if ((size = libebg.env_get(&ebgenv, (char *)name, NULL)) != 0) {
			value = malloc(size);
			if (value) {
				if (libebg.env_get(&ebgenv, (char *)name, value) != 0) {
					value = NULL;
				}
			}
		}
	}

	(void)libebg.env_close(&ebgenv);

	if (value == NULL) {
		ERROR("Cannot get %s from bootloader environment: %s",
		    name, strerror(errno));
	}

	/* Map EFI Boot Guard's int return to update_state_t's char value */
	*value = *value + '0';
	return value;
}

static int do_apply_list(const char *filename)
{
	FILE *fp = NULL;
	char *line = NULL;
	char *key;
	char *value;
	size_t len = 0;
	int ret = 0;

	errno = 0;
	libebg.beverbose(&ebgenv, loglevel > INFOLEVEL ? true : false);

	if (!(fp = fopen(filename, "rb"))) {
		ERROR("Failed to open bootloader environment file %s: %s",
			      filename, strerror(errno));
		return -1;
	}

	while ((getline(&line, &len, fp)) != -1) {
		key = strtok(line, "=");
		value = strtok(NULL, "\t\n");
		if (value != NULL && key != NULL) {
			if ((ret = bootloader_env_set(key, value)) != 0) {
				break;
			}
		}
	}

	if (fp) {
		fclose(fp);
	}
	if (line) {
		free(line);
	}
	return ret;
}

static bootloader ebg = {
	.env_get = &do_env_get,
	.env_set = &do_env_set,
	.env_unset = &do_env_unset,
	.apply_list = &do_apply_list
};

static bootloader* probe(void)
{
	void* handle = dlopen("libebgenv.so", RTLD_NOW | RTLD_GLOBAL);
	if (!handle) {
		return NULL;
	}

	(void)dlerror();
	load_symbol(handle, &libebg.beverbose, "ebg_beverbose");
	load_symbol(handle, &libebg.env_create_new, "ebg_env_create_new");
	load_symbol(handle, &libebg.env_open_current, "ebg_env_open_current");
	load_symbol(handle, &libebg.env_get, "ebg_env_get");
	load_symbol(handle, &libebg.env_set, "ebg_env_set");
	load_symbol(handle, &libebg.env_set_ex, "ebg_env_set_ex");
	load_symbol(handle, &libebg.env_getglobalstate, "ebg_env_getglobalstate");
	load_symbol(handle, &libebg.env_setglobalstate, "ebg_env_setglobalstate");
	load_symbol(handle, &libebg.env_close, "ebg_env_close");
	load_symbol(handle, &libebg.env_finalize_update, "ebg_env_finalize_update");
	return &ebg;
}

__attribute__((constructor))
static void ebg_probe(void)
{
	(void)register_bootloader("ebg", probe());
}
