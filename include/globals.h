/*
 * (C) Copyright 2014
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
 * Foundation, Inc. 
 */

#ifndef _GLOBALS_H
#define _GLOBALS_H

#define BANNER "Swupdate v" SWU_VER "\n"

#define SWUPDATE_GENERAL_STRING_SIZE	256
#define MAX_IMAGE_FNAME	SWUPDATE_GENERAL_STRING_SIZE
#define MAX_URL		SWUPDATE_GENERAL_STRING_SIZE
#define MAX_VOLNAME	SWUPDATE_GENERAL_STRING_SIZE
#define MAX_HW_VERSIONS	10
#define MAX_LINE	80
#define BOOTLOADER_VAR_LENGTH 16
#define MAX_REVISION_LENGTH	SWUPDATE_GENERAL_STRING_SIZE
#define MAX_BOOT_SCRIPT_LINE_LENGTH	1024
#define MAX_SEEK_STRING_SIZE	32

/* These are fixed path to temporary files */
#define SCRIPTS_DIR_SUFFIX	"scripts/"
#define DATADST_DIR_SUFFIX	"datadst/"
#define BOOT_SCRIPT_SUFFIX	"boot-script"

#endif

