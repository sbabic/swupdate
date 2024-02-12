/*
 * (C) Copyright 2013
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */


#pragma once

#include <stdbool.h>
#include "swupdate.h"
#include "handler.h"
#include "cpiohdr.h"

swupdate_file_t check_if_required(struct imglist *list, struct filehdr *pfdh,
				const char *destdir,
				struct img_type **pimg);
int install_images(struct swupdate_cfg *sw);
int install_single_image(struct img_type *img, bool dry_run);
int install_from_file(const char *filename, bool check);
int postupdate(struct swupdate_cfg *swcfg, const char *info);
int preupdatecmd(struct swupdate_cfg *swcfg);
int run_prepost_scripts(struct imglist *list, script_fn type);
void cleanup_files(struct swupdate_cfg *software);
