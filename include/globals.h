/*
 * (C) Copyright 2014
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * See file CREDITS for list of people who contributed to this
 * project.
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

#define BANNER "Swupdate v" SWU_VER "\n" \
	"Built on " AUTOCONF_TIMESTAMP

#define SWUPDATE_GENERAL_STRING_SIZE	256
#define MAX_IMAGE_FNAME	SWUPDATE_GENERAL_STRING_SIZE
#define MAX_URL		SWUPDATE_GENERAL_STRING_SIZE
#define MAX_VOLNAME	SWUPDATE_GENERAL_STRING_SIZE
#define MAX_HW_VERSIONS	10
#define MAX_LINE	80
#define UBOOT_VAR_LENGTH 16
#define MAX_REVISION_LENGTH	SWUPDATE_GENERAL_STRING_SIZE

/* These are fixed path to temporary files */
#define SCRIPTS_DIR	TMPDIR "scripts/"
#define DATASRC_DIR	TMPDIR "datasrc/"
#define DATADST_DIR	TMPDIR "datadst/"
#define UBOOT_SCRIPT	TMPDIR "uboot-script"

#endif

