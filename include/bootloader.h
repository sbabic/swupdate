/*
 * (C) Copyright 2017
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once
#include <stdbool.h>

#define BOOTLOADER_EBG   "ebg"
#define BOOTLOADER_NONE  "none"
#define BOOTLOADER_GRUB  "grub"
#define BOOTLOADER_UBOOT "uboot"
#define BOOTLOADER_CBOOT "cboot"

#define load_symbol(handle, container, fname) \
	*(void**)(container) = dlsym(handle, fname); \
	if (dlerror() != NULL) { \
		(void)dlclose(handle); \
		return NULL; \
	}

typedef struct {
	int (*env_set)(const char *, const char *);
	int (*env_unset)(const char *);
	char* (*env_get)(const char *);
	int (*apply_list)(const char *);
} bootloader;

/*
 * register_bootloader - register bootloader.
 *
 * @name : bootloader's name to register.
 * @bootloader : struct bootloader with bootloader details.
 *
 * Return:
 *   0 on success, -ENOMEM on error.
 */
int register_bootloader(const char *name, bootloader *bl);

/*
 * set_bootloader - set bootloader to use.
 *
 * @name : bootloader's name to set.
 *
 * Return:
 *   0 on success, -ENOENT on error.
 */
int set_bootloader(const char *name);

/*
 * get_bootloader - get set bootloader's name
 *
 * Return:
 *   name on success, NULL on error.
 */
const char* get_bootloader(void);

/*
 * is_bootloader - Test whether bootloader is currently selected
 *
 * @name : bootloader name to check if it's the currently selected one
 *
 * Return:
 *   true if name is currently selected bootloader, false otherwise
 */
bool is_bootloader(const char *name);

/*
 * print_registered_bootloaders - print registered bootloaders
 */
void print_registered_bootloaders(void);


/*
 * bootloader_env_set - modify a variable
 *
 * @name : variable
 * @value : value to be set
 *
 * Return:
 *   0 on success
 */
extern int (*bootloader_env_set)(const char *, const char *);

/*
 * bootloader_env_unset - drop a variable
 *
 * @name : variable
 *
 * Return:
 *   0 on success
 */
extern int (*bootloader_env_unset)(const char *);

/*
 * bootloader_env_get - get value of a variable
 *
 * @name : variable
 *
 * Return:
 *   string if variable is found
 *   NULL if no variable with name is found
 */
extern char* (*bootloader_env_get)(const char *);

/*
 * bootloader_apply_list - set multiple variables
 *
 * @filename : file in format <variable>=<value>
 *
 * Return:
 *   0 on success
 */
extern int (*bootloader_apply_list)(const char *);

