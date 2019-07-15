/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>

#include "swupdate.h"
#include "handler.h"
#include "lua_util.h"

#define MAX_INSTALLER_HANDLER	64
struct installer_handler supported_types[MAX_INSTALLER_HANDLER];
static unsigned long nr_installers = 0;
static unsigned long handler_index = ULONG_MAX;

int register_handler(const char *desc,
		handler installer, HANDLER_MASK mask, void *data)
{

	if (nr_installers > MAX_INSTALLER_HANDLER - 1)
		return -1;

	strncpy(supported_types[nr_installers].desc, desc,
		      sizeof(supported_types[nr_installers].desc));
	supported_types[nr_installers].installer = installer;
	supported_types[nr_installers].data = data;
	supported_types[nr_installers].mask = mask;
	nr_installers++;

	return 0;
}

void print_registered_handlers(void)
{
	unsigned int i;

	if (!nr_installers)
		return;

	printf("Registered handlers:\n");
	for (i = 0; i < nr_installers; i++) {
		printf("\t%s\n", supported_types[i].desc);
	}
}

struct installer_handler *find_handler(struct img_type *img)
{
	unsigned int i;

	for (i = 0; i < nr_installers; i++) {
		if ((strlen(img->type) == strlen(supported_types[i].desc)) &&
			strcmp(img->type,
				supported_types[i].desc) == 0)
			break;
	}
	if (i >= nr_installers)
		return NULL;
	return &supported_types[i];
}

struct installer_handler *get_next_handler(void)
{
	if (handler_index == ULONG_MAX) {
		handler_index = 0;
	}
	if (handler_index >= nr_installers) {
		handler_index = ULONG_MAX;
		return NULL;
	}
	return &supported_types[handler_index++];
}

unsigned int get_handler_mask(struct img_type *img)
{
	struct installer_handler *hnd;
	unsigned int mask = 0;

	hnd = find_handler(img);

	if (hnd)
		mask = hnd->mask;

	return mask;
}
