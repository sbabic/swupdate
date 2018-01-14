/*
 * (C) Copyright 2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <unistd.h>
#include "bootloader.h"
int bootloader_env_set(const char __attribute__ ((__unused__)) *name,
			const char __attribute__ ((__unused__)) *value)
{
	return 0;
}

int bootloader_env_unset(const char __attribute__ ((__unused__)) *name)
{
	return 0;
}

char *bootloader_env_get(const char __attribute__ ((__unused__)) *name)
{
	return NULL;
}

int bootloader_apply_list(const char __attribute__ ((__unused__)) *filename)
{
	return 0;
}
