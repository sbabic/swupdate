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
#include "bootloader.h"

static ebgenv_t ebgenv = {0};

int bootloader_env_set(const char *name, const char *value)
{
	int ret;

	errno = 0;
	ebg_beverbose(&ebgenv, loglevel > INFOLEVEL ? true : false);

	DEBUG("Setting %s=%s in bootloader environment", name, value);

	if ((ret = ebg_env_open_current(&ebgenv)) != 0) {
		ERROR("Cannot open current bootloader environment: %s.", strerror(ret));
		return ret;
	}

	if (strncmp(name, BOOTVAR_TRANSACTION, strlen(name) + 1) == 0 &&
	    strncmp(value, get_state_string(STATE_IN_PROGRESS),
		    strlen(get_state_string(STATE_IN_PROGRESS)) + 1) == 0) {
		/* Open or create a new environment to reflect
		 * EFI Boot Guard's representation of SWUpdate's
		 * recovery_status=in_progress. */
		if ((ret = ebg_env_create_new(&ebgenv)) != 0) {
			ERROR("Cannot open/create new bootloader environment: %s.",
			     strerror(ret));
		}
	} else if (strncmp(name, (char *)STATE_KEY, strlen((char *)STATE_KEY) + 1) == 0) {
		/* Map suricatta's update_state_t to EFI Boot Guard's API. */
		if ((ret = ebg_env_setglobalstate(&ebgenv, *value - '0')) != 0) {
			ERROR("Cannot set %s=%s in bootloader environment.", STATE_KEY, value);
		}
	} else {
		/* A new environment is created if EFI Boot Guard's
		 * representation of SWUpdate's recovery_status is
		 * not in_progress. */
		if ((ret = ebg_env_create_new(&ebgenv)) != 0) {
			ERROR("Cannot open/create new bootloader environment: %s.",
			     strerror(ret));
			return ret;
		}
		if ((ret = ebg_env_set(&ebgenv, (char *)name, (char *)value)) != 0) {
			ERROR("Cannot set %s=%s in bootloader environment: %s.",
			    name, value, strerror(ret));
		}
	}
	(void)ebg_env_close(&ebgenv);

	return ret;
}

int bootloader_env_unset(const char *name)
{
	int ret;

	ebg_beverbose(&ebgenv, loglevel > INFOLEVEL ? true : false);

	if ((ret = ebg_env_open_current(&ebgenv)) != 0) {
		ERROR("Cannot open current bootloader environment: %s.", strerror(ret));
		return ret;
	}

	if (strncmp(name, BOOTVAR_TRANSACTION, strlen(name) + 1) == 0) {
		ret = ebg_env_finalize_update(&ebgenv);
		if (ret) {
			ERROR("Cannot unset %s in bootloader environment: %s.", BOOTVAR_TRANSACTION, strerror(ret));
		}
	} else if (strncmp(name, (char *)STATE_KEY, strlen((char *)STATE_KEY) + 1) == 0) {
		/* Unsetting STATE_KEY is semantically equivalent to setting it to STATE_OK. */
		if ((ret = ebg_env_setglobalstate(&ebgenv, STATE_OK - '0')) != 0) {
			ERROR("Cannot unset %s in bootloader environment.", STATE_KEY);
		}
	} else {
		ret = ebg_env_set_ex(&ebgenv, (char *)name, USERVAR_TYPE_DELETED, (uint8_t *)"", 1);
	}
	(void)ebg_env_close(&ebgenv);

	return ret;
}

char *bootloader_env_get(const char *name)
{
	char *value = NULL;
	size_t size;

	errno = 0;
	ebg_beverbose(&ebgenv, loglevel > INFOLEVEL ? true : false);

	int ret;
	if ((ret = ebg_env_open_current(&ebgenv)) != 0) {
		ERROR("Cannot open current bootloader environment: %s.",
		     strerror(ret));
		return NULL;
	}

	if (strncmp(name, (char *)STATE_KEY, strlen((char *)STATE_KEY) + 1) == 0) {
		value = (char *)malloc(sizeof(char));
		*value = ebg_env_getglobalstate(&ebgenv);
	} else {
		if ((size = ebg_env_get(&ebgenv, (char *)name, NULL)) != 0) {
			value = malloc(size);
			if (value) {
				if (ebg_env_get(&ebgenv, (char *)name, value) != 0) {
					value = NULL;
				}
			}
		}
	}

	(void)ebg_env_close(&ebgenv);

	if (value == NULL) {
		ERROR("Cannot get %s from bootloader environment: %s",
		    name, strerror(errno));
	}

	/* Map EFI Boot Guard's int return to update_state_t's char value */
	*value = *value + '0';
	return value;
}

int bootloader_apply_list(const char *filename)
{
	FILE *fp = NULL;
	char *line = NULL;
	char *key;
	char *value;
	size_t len = 0;
	int ret = 0;

	errno = 0;
	ebg_beverbose(&ebgenv, loglevel > INFOLEVEL ? true : false);

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
