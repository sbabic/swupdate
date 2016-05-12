/*
 * (C) Copyright 2016
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

/*
 * this file just contains the required structures to extract
 * files from a cpio archive. It does not have all structures
 * but just what is needed by swupdate
 */

#ifndef _CPIOHDR_SWUPD_H
#define _CPIOHDR_SWUPD_H

/* Global swupdate defines */
#include "globals.h"

/*
 * cpio header - swupdate does not
 * support images generated with ancient cpio.
 * Just the new format as described in cpio
 * documentation is supported.
 */

struct new_ascii_header
{
  char c_magic[6];
  char c_ino[8];
  char c_mode[8];
  char c_uid[8];
  char c_gid[8];
  char c_nlink[8];
  char c_mtime[8];
  char c_filesize[8];
  char c_dev_maj[8];
  char c_dev_min[8];
  char c_rdev_maj[8];
  char c_rdev_min[8];
  char c_namesize[8];
  char c_chksum[8];
};

struct filehdr {
	long size;
	long namesize;
	long chksum;
	char filename[MAX_IMAGE_FNAME];
};

int extract_cpio_header(int fd, struct filehdr *fhdr, unsigned long *offset);
int extract_img_from_cpio(int fd, unsigned long offset, struct filehdr *fdh);

#endif
