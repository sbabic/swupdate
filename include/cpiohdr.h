/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
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
	unsigned long size;
	unsigned long namesize;
	unsigned long chksum;
	char filename[MAX_IMAGE_FNAME];
};

int get_cpiohdr(unsigned char *buf, unsigned long *size,
			unsigned long *namesize, unsigned long *chksum);
int extract_cpio_header(int fd, struct filehdr *fhdr, unsigned long *offset);
int extract_img_from_cpio(int fd, unsigned long offset, struct filehdr *fdh);
void extract_padding(int fd, unsigned long *offset);

#endif
