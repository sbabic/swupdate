/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc. 
 */


#ifndef _INSTALLER_H
#define _INSTALLER_H

#include "swupdate.h"
#include "handler.h"
#include "cpiohdr.h"

int check_if_required(struct imglist *list, struct filehdr *pfdh,
				struct swver *sw_ver_list,
				struct img_type **pimg);
int install_images(struct swupdate_cfg *sw, int fdsw, int fromfile);
int install_single_image(struct img_type *img);
int run_prepost_scripts(struct swupdate_cfg *sw, script_fn type);
int postupdate(struct swupdate_cfg *swcfg, const char *info);
void cleanup_files(struct swupdate_cfg *software);

#endif
