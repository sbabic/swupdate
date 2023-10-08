/*
 * (C) Copyright 2023
 * Stefano Babic, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _SWUPDATE_HW_COMPATIBILITY_H
#define _SWUPDATE_HW_COMPATIBILITY_H

#include "bsdqueue.h"
#include "globals.h"

struct hw_type {
	char boardname[SWUPDATE_GENERAL_STRING_SIZE];
	char revision[SWUPDATE_GENERAL_STRING_SIZE];
	LIST_ENTRY(hw_type) next;
};

LIST_HEAD(hwlist, hw_type);

int check_hw_compatibility(struct hw_type *hwt, struct hwlist *hardware);
int get_hw_revision(struct hw_type *hw);

#endif

