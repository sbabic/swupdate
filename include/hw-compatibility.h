/*
 * (C) Copyright 2013-2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once
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
