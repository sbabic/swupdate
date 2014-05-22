/*
 * (C) Copyright 2012-2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * See file CREDITS for list of people who contributed to this
 * project.
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#ifndef _UTIL_H
#define _UTIL_H

#include <stdint.h>
#include "swupdate.h"

extern int verbose;

typedef enum {
	IDLE,
	START,
	RUN,
	SUCCESS,
	FAILURE
} RECOVERY_STATUS;

enum {
	RECOVERY_NO_ERROR,
	RECOVERY_ERROR,
	WRITE_ERROR,
	FAILED_OPEN,
	REQUIRED_FILE_NOT_FOUND,
	WRONG_COMPRESS_IMAGE,
	OUT_OF_MEMORY,
	LUA_SCRIPT_LOAD,
	LUA_SCRIPT_PREPARE,
	LUA_SCRIPT_RUN,
	LUA_MISMATCH_TYPE,
	UBI_TOO_BIG_VOLUME,
	UBI_NOT_VOLUME,
	UBI_WRITE_VOLUME,
	INTERNAL_ERROR,
	NO_MTD_DEV,
	MOUNT_FAILED
};

struct installer {
	int	fd;				//!< install image file handle
	RECOVERY_STATUS	status;			//!< "idle" or "request source" info
	RECOVERY_STATUS	last_install;		//!< result from last installation
	int	last_error;			//!< error code if installation failed
	char	errormsg[64];			//!< error message if installation failed
};

typedef void (*notifier) (RECOVERY_STATUS status, int error, const char *msg);

#define TRACE(format, arg...) do { \
	char tmpbuf[1024]; \
	if (verbose) { \
		snprintf(tmpbuf, sizeof(tmpbuf), "[%s] : " format, __func__, ## arg); \
		notify(RUN, RECOVERY_NO_ERROR, tmpbuf); \
	} \
} while(0)

#define ERROR(format, arg...) do { \
	char tmpbuf[128]; \
	if (verbose) \
		snprintf(tmpbuf, sizeof(tmpbuf), "ERROR %s : %s : %d : " format, __FILE__, __func__, __LINE__, ## arg); \
	else \
		snprintf(tmpbuf, sizeof(tmpbuf), "ERROR : " format, ## arg); \
	fprintf(stderr, "%s\n", tmpbuf); \
	notify(FAILURE, 0, tmpbuf); \
} while(0)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define TMPDIR		"/tmp/"

#define LG_16 4
#define FROM_HEX(f) from_ascii (f, sizeof f, LG_16)

int gpio_export(int gpio);
int gpio_unexport(int gpio);
int gpio_direction_input(int gpio_number);
int gpio_direction_output(int gpio_number, int value);
int gpio_set_value(int gpio_number, int value);
int gpio_get_value(int gpio_number);

int fill_buffer(int fd, unsigned char *buf, int nbytes, unsigned long *offs,
	uint32_t *checksum);
int decompress_image(int infile, unsigned long *offs, int nbytes,
	int outfile, uint32_t *checksum);
int fw_set_one_env(const char *name, const char *value);
int openfile(const char *filename);
int copyfile(int fdin, int fdout, int nbytes, unsigned long *offs,
	int skip_file, int compressed, uint32_t *checksum);
off_t extract_sw_description(int fd);
off_t extract_next_file(int fd, int fdout, off_t start, int compressed);
int openfileoutput(const char *filename);

int register_notifier(notifier client);
void notify(RECOVERY_STATUS status, int error, const char *msg);
void notify_init(void);

char **splitargs(char *args, int *argc);
void freeargs (char **argv);
int isDirectoryEmpty(const char *dirname);
int get_hw_revision(struct hw_type *hw);
int check_hw_compatibility(struct swupdate_cfg *cfg);
#endif
