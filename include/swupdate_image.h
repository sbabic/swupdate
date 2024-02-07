/*
 * (C) Copyright 2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <sys/types.h>
#include <stdbool.h>
#include "bsdqueue.h"
#include "globals.h"
#include "swupdate_dict.h"
#include "lua_util.h"

typedef enum {
	FLASH,
	UBI,
	FILEDEV,
	PARTITION,
	SCRIPT
} imagetype_t;

typedef enum {
	SKIP_NONE=0,
	SKIP_SAME,
	SKIP_HIGHER,
	SKIP_SCRIPT
} skip_t;

struct sw_version {
	char name[SWUPDATE_GENERAL_STRING_SIZE];
	char version[SWUPDATE_GENERAL_STRING_SIZE];
	int install_if_different;
	int install_if_higher;
	LIST_ENTRY(sw_version) next;
};

LIST_HEAD(swver, sw_version);

struct img_type {
	struct sw_version id;		/* This is used to compare versions */
	char type[SWUPDATE_GENERAL_STRING_SIZE]; /* Handler name */
	char fname[MAX_IMAGE_FNAME];	/* Filename in CPIO archive */
	char volname[MAX_VOLNAME];	/* Useful for UBI	*/
	char device[MAX_VOLNAME];	/* device associated with image if any */
	char path[MAX_IMAGE_FNAME];	/* Path where image must be installed */
	char mtdname[MAX_IMAGE_FNAME];	/* MTD device where image must be installed */
	char type_data[SWUPDATE_GENERAL_STRING_SIZE];	/* Data for handler */
	char extract_file[MAX_IMAGE_FNAME];
	char filesystem[MAX_IMAGE_FNAME];
	unsigned long long seek;
	skip_t skip;
	int provided;
	int compressed;
	int preserve_attributes; /* whether to preserve attributes in archives */
	bool is_encrypted;
	char ivt_ascii[33];
	int install_directly;
	int is_script;
	int is_partitioner;
	struct dict properties;

	/*
	 * Pointers to global structures
	 * that are alive during an installation. They can be used by handlers
	 */
	struct dict *bootloader;/* pointer to swupdate_cfg's bootloader dict for handlers to modify */
	lua_State *L;		/* pointer to swupdate_cfg's LUa state created by parser */

	long long partsize;
	int fdin;	/* Used for streaming file */
	off_t offset;	/* offset in cpio file */
	long long size;
	unsigned int checksum;
	unsigned char sha256[SHA256_HASH_LENGTH];	/* SHA-256 is 32 byte */
	LIST_ENTRY(img_type) next;
};

LIST_HEAD(imglist, img_type);
