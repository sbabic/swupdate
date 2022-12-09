/*
 * Author: Christian Storm
 * Copyright (C) 2022, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */
#include <stdlib.h>
#include <errno.h>
#include <util.h>
#include <bootloader.h>

int   (*bootloader_env_set)(const char *, const char *);
int   (*bootloader_env_unset)(const char *);
char* (*bootloader_env_get)(const char *);
int   (*bootloader_apply_list)(const char *);

typedef struct {
	const char *name;
	bootloader *funcs;
} entry;

static entry *current = NULL;
static entry *available = NULL;
static unsigned int num_available = 0;

int register_bootloader(const char *name, bootloader *bl)
{
	entry *tmp = realloc(available, (num_available + 1) * sizeof(entry));
	if (!tmp) {
		return -ENOMEM;
	}
	tmp[num_available].name = (char*)name;
	tmp[num_available].funcs = bl;
	num_available++;
	available = tmp;
	return 0;
}

int set_bootloader(const char *name)
{
	if (!name) {
		return -ENOENT;
	}
	for (unsigned int i = 0; i < num_available; i++) {
		if (available[i].funcs &&
		    (strcmp(available[i].name, name) == 0)) {
			bootloader_env_set = available[i].funcs->env_set;
			bootloader_env_get = available[i].funcs->env_get;
			bootloader_env_unset = available[i].funcs->env_unset;
			bootloader_apply_list = available[i].funcs->apply_list;
			current = &available[i];
			return 0;
		}
	}
	return -ENOENT;
}

bool is_bootloader(const char *name) {
	if (!name || !current) {
		return false;
	}
	return strcmp(current->name, name) == 0;
}

const char* get_bootloader(void)
{
	return current ? current->name : NULL;
}

void print_registered_bootloaders(void)
{
	TRACE("Registered bootloaders:");
	for (unsigned int i = 0; i < num_available; i++) {
		TRACE("\t%s\t%s", available[i].name,
		     available[i].funcs == NULL ? "shared lib not found."
						: "loaded.");
	}
}
