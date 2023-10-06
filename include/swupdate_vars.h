/*
 * (C) Copyright 2023
 * Stefano Babic, <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _SWVARS_H
#define _SWVARS_H

#include <libuboot.h>

int swupdate_vars_initialize(struct uboot_ctx **ctx, const char *namespace);
int swupdate_vars_apply_list(const char *filename, const char *namespace);
char *swupdate_vars_get(const char *name, const char *namespace);
int swupdate_vars_set(const char *name, const char *value, const char *namespace);
int swupdate_vars_unset(const char *name, const char *namespace);

#endif
