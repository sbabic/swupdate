/*
 * (C) Copyright 2012-2013
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */


#pragma once

struct img_type;

/*
 * Identify the type of a script
 * A script can contain functions for several of these
 * entries. For example, it can contain both a pre- and post-
 * install functions.
 */
typedef enum {
	NONE,
	PREINSTALL,	/* Script runs as preinstall */
	POSTINSTALL,	/* Script runs a postinstall */
	POSTFAILURE	/* Script called in case update fails */
} script_fn ;

/*
 * Use enum for mask to easy transfer to Lua
 * scripts
 */
typedef enum {
	IMAGE_HANDLER = 1,
	FILE_HANDLER = 2,
	SCRIPT_HANDLER = 4,
	BOOTLOADER_HANDLER = 8,
	PARTITION_HANDLER = 16,
	NO_DATA_HANDLER = 32
} HANDLER_MASK;

#define ANY_HANDLER (IMAGE_HANDLER | FILE_HANDLER | SCRIPT_HANDLER | \
			BOOTLOADER_HANDLER | PARTITION_HANDLER | \
			NO_DATA_HANDLER)

typedef int (*handler)(struct img_type *img, void *data);
struct installer_handler{
	char	desc[64];
	handler installer;
	void	*data;
	unsigned int mask;
};

struct script_handler_data {
	/*
	 * scriptfn must be first, as script handlers may expect to
	 * receive just a script_fn
	 */
	script_fn scriptfn;
	void	*data;
};

int register_handler(const char *desc, 
		handler installer, HANDLER_MASK mask, void *data);

struct installer_handler *find_handler(struct img_type *img);
void print_registered_handlers(void);
struct installer_handler *get_next_handler(void);
unsigned int get_handler_mask(struct img_type *img);
