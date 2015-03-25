/*
 * (C) Copyright 2012
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/fcntl.h>

#include "autoconf.h"
#include "cpiohdr.h"
#include "util.h"
#include "swupdate.h"
#include "parsers.h"

#define MODULE_NAME "cpio"

#define LG_16 4
#define FROM_HEX(f) from_ascii (f, sizeof f, LG_16)
#define BUFF_SIZE	 16384

#define NPAD_BYTES(o) ((4 - (o % 4)) % 4)

static unsigned char in[BUFF_SIZE];

static uintmax_t
from_ascii (char const *where, size_t digs, unsigned logbase)
{
	uintmax_t value = 0;
	char const *buf = where;
	char const *end = buf + digs;
	int overflow = 0;
	static char codetab[] = "0123456789ABCDEF";

	for (; *buf == ' '; buf++)
	{
		if (buf == end)
		return 0;
	}

	if (buf == end || *buf == 0)
		return 0;
	while (1)
	{
		unsigned d;

		char *p = strchr (codetab, toupper (*buf));
		if (!p)
		{
			TRACE("Malformed number %.*s\n", (int)digs, where);
			break;
		}

		d = p - codetab;
		if ((d >> logbase) > 1)
		{
			TRACE("Malformed number %.*s\n", (int)digs, where);
			break;
		}
		value += d;
		if (++buf == end || *buf == 0)
			break;
		overflow |= value ^ (value << logbase >> logbase);
		value <<= logbase;
	}
	if (overflow)
		TRACE ("Archive value %.*s is out of range\n",
			(int)digs, where);
	return value;
}

static int get_cpiohdr(unsigned char *buf, long *size, long *namesize, long *chksum)
{
	struct new_ascii_header *cpiohdr;

	cpiohdr = (struct new_ascii_header *)buf;
	if (strncmp(cpiohdr->c_magic, "070702", 6) != 0) {
		TRACE("CPIO Format not recognized: magic not found\n");
			return -EINVAL;
	}
	*size = FROM_HEX(cpiohdr->c_filesize);
	*namesize = FROM_HEX(cpiohdr->c_namesize);
	*chksum =  FROM_HEX(cpiohdr->c_chksum);

	return 0;
}

int fill_buffer(int fd, unsigned char *buf, int nbytes, unsigned long *offs,
	uint32_t *checksum)
{
	unsigned long len;
	unsigned long count = 0;
	int i;

	while (nbytes > 0) {
		len = read(fd, buf, nbytes);
		if (len < 0) {
			TRACE("Failure in stream: I cannot go on\n");
			return -EFAULT;
		}
		if (len == 0) {
			return 0;
		}
		if (checksum)
			for (i = 0; i < len; i++)
				*checksum += buf[i];

		buf += len;
		count += len;
		nbytes -= len;
		*offs += len;
	}
	return count;
}

static int copy_write(int fd, const void *buf, int len)
{
	int ret;

	while (len) {
		ret = write(fd, buf, len);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			ERROR("cannot write %d bytes", len);
			return -1;
		}

		if (ret == 0) {
			ERROR("cannot write %d bytes", len);
			return -1;
		}

		len -= ret;
		buf += ret;
	}

	return 0;
}

int copyfile(int fdin, int fdout, int nbytes, unsigned long *offs,
	int skip_file, int __attribute__ ((__unused__)) compressed,
	uint32_t *checksum)
{
	unsigned long size;
	int ret;

	if (checksum)
		*checksum = 0;


#ifdef CONFIG_GUNZIP
	if (compressed) {
		ret = decompress_image(fdin, offs, nbytes, fdout, checksum);
		if (ret < 0) {
			ERROR("wrong gzip image -- aborting\n");
			return ret;
		}
		return 0;
	}
#endif

	while (nbytes > 0) {
		size = (nbytes < BUFF_SIZE ? nbytes : BUFF_SIZE);

		if ((ret = fill_buffer(fdin, in, size, offs, checksum) < 0)) {
			close(fdout);
			return ret;
		}

		nbytes -= size;
		if (skip_file)
			continue;

		if (copy_write(fdout, in, size) < 0)
				return(-ENOSPC);
	}

	fill_buffer(fdin, in, NPAD_BYTES(*offs), offs, checksum);

	return 0;
}

int extract_cpio_header(int fd, struct filehdr *fhdr, unsigned long *offset)
{
	unsigned char buf[256];
	if (fill_buffer(fd, buf, sizeof(struct new_ascii_header), offset, NULL) < 0)
		return(-EINVAL);
	if (get_cpiohdr(buf, &fhdr->size, &fhdr->namesize, &fhdr->chksum) < 0) {
		ERROR("CPIO Header corrupted, cannot be parsed");
		return -EINVAL;
	}
	if (fhdr->namesize >= sizeof(fhdr->filename))
	{
		ERROR("CPIO Header filelength too big %u >= %u (max)",
			(unsigned int)fhdr->namesize,
			(unsigned int)sizeof(fhdr->filename));
		return -EINVAL;
	}

	if (fill_buffer(fd, buf, fhdr->namesize , offset, NULL) < 0)
		return(-EINVAL);
	strncpy(fhdr->filename, (char *)buf, sizeof(fhdr->filename));

	/* Skip filename padding, if any */
	if (fill_buffer(fd, buf, (4 - (*offset % 4)) % 4, offset, NULL) < 0)
		return(-EINVAL);

	return 0;
}

off_t extract_sw_description(int fd)
{
	struct filehdr fdh;
	unsigned long offset = 0;
	char output_file[64];
	uint32_t checksum;
	int fdout;

	if (extract_cpio_header(fd, &fdh, &offset)) {
		ERROR("CPIO Header wrong\n");
		return -1;
	}

	if (strcmp(fdh.filename, SW_DESCRIPTION_FILENAME)) {
		ERROR("%s not the first of the list: %s instead of %s\n",
			SW_DESCRIPTION_FILENAME,
			fdh.filename,
			SW_DESCRIPTION_FILENAME);
		return -1;
	}
	if ((strlen(TMPDIR) + strlen(fdh.filename)) > sizeof(output_file)) {
		ERROR("File Name too long : %s\n", fdh.filename);
		return -1;
	}
	strncpy(output_file, TMPDIR, sizeof(output_file));
	strcat(output_file, fdh.filename);
	fdout = openfileoutput(output_file);

	if (lseek(fd, offset, SEEK_SET) < 0) {
		ERROR("CPIO file corrupted : %s\n", strerror(errno));
		return -1;
	}
	if (copyfile(fd, fdout, fdh.size, &offset, 0, 0, &checksum) < 0) {
		ERROR("sw-description corrupted or not valid\n");
		return -1;
	}

	close(fdout);

	TRACE("Found file:\n\tfilename %s\n\tsize %u\n\tchecksum 0x%lx %s\n",
		fdh.filename,
		(unsigned int)fdh.size,
		(unsigned long)checksum,
		(checksum == fdh.chksum) ? "VERIFIED" : "WRONG");

	if (checksum != fdh.chksum) {
		ERROR("Checksum WRONG ! Computed 0x%lx, it should be 0x%lx\n",
			(unsigned long)checksum, fdh.chksum);
		return -1;
	}

	return offset;
}

int extract_img_from_cpio(int fd, unsigned long offset, struct filehdr *fdh)
{

	if (lseek(fd, offset, SEEK_SET) < 0) {
		ERROR("CPIO file corrupted : %s\n",
		strerror(errno));
		return -EBADF;
	}
	if (extract_cpio_header(fd, fdh, &offset)) {
		ERROR("CPIO Header wrong\n");
		return -1;
	}
	if (lseek(fd, offset, SEEK_SET) < 0) {
		ERROR("CPIO file corrupted : %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

off_t extract_next_file(int fd, int fdout, off_t start, int compressed)
{
	struct filehdr fdh;
	uint32_t checksum = 0;
	unsigned long offset = start;

	if (lseek(fd, offset, SEEK_SET) < 0) {
		ERROR("CPIO file corrupted : %s\n",
		strerror(errno));
		return -1;
	}

	if (extract_cpio_header(fd, &fdh, &offset)) {
		ERROR("CPIO Header wrong\n");
	}

	if (lseek(fd, offset, SEEK_SET) < 0)
		ERROR("CPIO file corrupted : %s\n", strerror(errno));
	if (copyfile(fd, fdout, fdh.size, &offset, 0, compressed, &checksum) < 0) {
		ERROR("Error copying extracted file\n");
	}

	TRACE("Copied file:\n\tfilename %s\n\tsize %u\n\tchecksum 0x%lx %s\n",
		fdh.filename,
		(unsigned int)fdh.size,
		(unsigned long)checksum,
		(checksum == fdh.chksum) ? "VERIFIED" : "WRONG");

	if (checksum != fdh.chksum)
		ERROR("Checksum WRONG ! Computed 0x%lx, it should be 0x%lx\n",
			(unsigned long)checksum, fdh.chksum);

	return offset;
}

int cpio_scan(int fd, struct swupdate_cfg *cfg, off_t start)
{
	struct filehdr fdh;
	unsigned long offset = start;
	int file_listed;
	uint32_t checksum;


	while (1) {
		file_listed = 0;
		start = offset;
		if (extract_cpio_header(fd, &fdh, &offset)) {
			return -1;
		}
		if (strcmp("TRAILER!!!", fdh.filename) == 0) {
			return offset;
		}

		SEARCH_FILE(struct img_type, cfg->images,
			file_listed, start);
		SEARCH_FILE(struct img_type, cfg->files,
			file_listed, start);
		SEARCH_FILE(struct img_type, cfg->scripts,
			file_listed, start);

		TRACE("Found file:\n\tfilename %s\n\tsize %d\n\t%s\n",
			fdh.filename,
			(unsigned int)fdh.size,
			file_listed ? "REQUIRED" : "not required");

		/*
		 * use copyfile for checksum verification, as we skip file
		 * we do not have to provide fdout
		 */
		if (copyfile(fd, 0, fdh.size, &offset, 1, 0, &checksum) != 0) {
			ERROR("invalid archive\n");
			return -1;
		}
		if ((uint32_t)(fdh.chksum) != checksum) {
			ERROR("Checksum verification failed for %s: %x != %x\n",
			fdh.filename, (uint32_t)fdh.chksum, checksum);
			return -1;
		}

		/* Next header must be 4-bytes aligned */
		offset += NPAD_BYTES(offset);
		if (lseek(fd, offset, SEEK_SET) < 0)
			ERROR("CPIO file corrupted : %s\n", strerror(errno));
	}

	return 0;
}

