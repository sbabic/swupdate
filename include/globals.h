/*
 * (C) Copyright 2014
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _GLOBALS_H
#define _GLOBALS_H

#define BANNER "Swupdate v" SWU_VER "\n"

#define SWUPDATE_GENERAL_STRING_SIZE	256
#define SWUPDATE_UPDATE_DESCRIPTION_STRING_SIZE	512
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

