/*
 * (C) Copyright 2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _BOOTLOADER_INTERFACE_H
#define _BOOTLOADER_INTERFACE_H

/*
 * bootloader_env_set - modify a variable
 *
 * @name : variable
 * @value : value to be set
 *
 * Return:
 *   0 on success
 */

int bootloader_env_set(const char *name, const char *value);

/*
 * bootloader_env_unset - drop a variable
 *
 * @name : variable
 *
 * Return:
 *   0 on success
 */

int bootloader_env_unset(const char *name);

/*
 * bootloader_env_get - get value of a variable
 *
 * @name : variable
 *
 * Return:
 *   string if variable is found
 *   NULL if no variable with name is found
 */

char *bootloader_env_get(const char *name);

/*
 * bootloader_apply_list - set multiple variables
 *
 * @filename : file in format <variable>=<value>
 *
 * Return:
 *   0 on success
 */
int bootloader_apply_list(const char *script);

#endif
