/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */


#ifndef _INSTALLER_H
#define _INSTALLER_H

#include "swupdate.h"
#include "handler.h"
#include "cpiohdr.h"

int check_if_required(struct imglist *list, struct filehdr *pfdh,
				const char *destdir,
				struct img_type **pimg);
int install_images(struct swupdate_cfg *sw, int fdsw, int fromfile);
int install_single_image(struct img_type *img, int dry_run);
int postupdate(struct swupdate_cfg *swcfg, const char *info);
void cleanup_files(struct swupdate_cfg *software);

#endif
