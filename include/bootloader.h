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
