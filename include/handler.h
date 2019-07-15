/*
 * (C) Copyright 2012-2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */


#ifndef _HANDLER_H
#define _HANDLER_H

typedef enum {
	NONE,
	PREINSTALL,
	POSTINSTALL
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

int register_handler(const char *desc, 
		handler installer, HANDLER_MASK mask, void *data);

struct installer_handler *find_handler(struct img_type *img);
void print_registered_handlers(void);
struct installer_handler *get_next_handler(void);
unsigned int get_handler_mask(struct img_type *img);

#endif
