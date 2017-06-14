/*
 * (C) Copyright 2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
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
