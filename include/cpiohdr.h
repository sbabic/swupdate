/*
 * (C) Copyright 2016
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * this file just contains the required structures to extract
 * files from a cpio archive. It does not have all structures
 * but just what is needed by swupdate
 */

#pragma once

/* Global swupdate defines */
#include <stdbool.h>
#include <sys/types.h>
#include "globals.h"
#include <stdint.h>

/*
 * cpio header - swupdate does not
 * support images generated with ancient cpio.
 * Just the new format as described in cpio
 * documentation is supported.
 */

#define CPIO_NEWASCII 070701
#define CPIO_CRCASCII 070702

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
	unsigned long format;
	unsigned long size;
	unsigned long namesize;
	unsigned long chksum;
	char filename[MAX_IMAGE_FNAME];
};

int get_cpiohdr(unsigned char *buf, struct filehdr *fhdr);
int extract_cpio_header(int fd, struct filehdr *fhdr, unsigned long *offset);
int extract_img_from_cpio(int fd, unsigned long offset, struct filehdr *fdh);
void extract_padding(int fd);
bool swupdate_verify_chksum(const uint32_t chk1, struct filehdr *fhdr);
