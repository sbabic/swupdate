/*
 * Author: Maciej Pijanowski, maciej.pijanowski@3mdeb.com
 * Copyright (C) 2017, 3mdeb
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _GRUBENV_H
#define _GRUBENV_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "util.h"
#include "swupdate_dict.h"


#define GRUBENV_SIZE 1024 /* bytes */
#define GRUBENV_HEADER "# GRUB Environment Block\n"
#define GRUBENV_DEFAULT_PATH "/boot/efi/EFI/BOOT/grub/grubenv"

#ifdef CONFIG_GRUBENV_PATH
#define GRUBENV_PATH	CONFIG_GRUBENV_PATH
#else
#define GRUBENV_PATH	GRUBENV_DEFAULT_PATH
#endif

#define GRUBENV_PATH_NEW	GRUBENV_PATH ".new"

struct grubenv_t {
	struct dict vars;
	size_t size;
};

/* only these should be called from external */
int grubenv_set(const char *name, const char *value);
int grubenv_unset(const char *name);
int grubenv_apply_list(const char *script);
char *grubenv_get(const char *name);

#endif
