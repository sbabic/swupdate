/*
 * (C) Copyright 2023
 * Stefano Babic, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _SWUPDATE_HW_COMPATIBILITY_H
#define _SWUPDATE_HW_COMPATIBILITY_H

#include "swupdate.h"

int check_hw_compatibility(struct swupdate_cfg *cfg);
int get_hw_revision(struct hw_type *hw);

#endif

