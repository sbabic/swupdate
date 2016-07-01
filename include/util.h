/*
 * (C) Copyright 2012-2016
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#ifndef _UTIL_H
#define _UTIL_H

#include <stdint.h>
#include "swupdate.h"
#include "swupdate_status.h"

extern int loglevel;

typedef enum {
	OFF,
	ERRORLEVEL,
	WARNLEVEL,
	INFOLEVEL,
	DEBUGLEVEL,
	TRACELEVEL
} LOGLEVEL;

enum {
	RECOVERY_NO_ERROR,
	RECOVERY_ERROR,
};

struct installer {
	int	fd;				//!< install image file handle
	RECOVERY_STATUS	status;			//!< "idle" or "request source" info
	RECOVERY_STATUS	last_install;		//!< result from last installation
	int	last_error;			//!< error code if installation failed
	char	errormsg[64];			//!< error message if installation failed
};

typedef void (*notifier) (RECOVERY_STATUS status, int level, const char *msg);

#define swupdate_notify(status, format, level, arg...) do { \
	if (loglevel >= level) { \
		char tmpbuf[1024]; \
		if (status == FAILURE) { \
			if (loglevel >= DEBUGLEVEL) \
				snprintf(tmpbuf, sizeof(tmpbuf), \
				     	"ERROR %s : %s : %d : " format, \
					       	__FILE__, \
					       	__func__, \
					       	__LINE__, \
						## arg); \
			else \
				snprintf(tmpbuf, sizeof(tmpbuf), \
					       	"ERROR : " format, ## arg); \
			fprintf(stderr, "%s\n", tmpbuf); \
			notify(FAILURE, 0, tmpbuf); \
		} else {\
			snprintf(tmpbuf, sizeof(tmpbuf), \
				       	"[%s] : " format, __func__, ## arg); \
			notify(RUN, RECOVERY_NO_ERROR, tmpbuf); \
		} \
	} \
} while(0)

#define ERROR(format, arg...) \
	swupdate_notify(FAILURE, format, ERRORLEVEL, ## arg)

#define WARN(format, arg...) \
	swupdate_notify(RUN, format, WARNLEVEL, ## arg)

#define INFO(format, arg...) \
	swupdate_notify(RUN, format, INFOLEVEL, ## arg)

#define TRACE(format, arg...) \
	swupdate_notify(RUN, format, TRACELEVEL, ## arg)

#define DEBUG(format, arg...) \
	swupdate_notify(RUN, format, DEBUGLEVEL, ## arg)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define TMPDIR		"/tmp/"

#define LG_16 4
#define FROM_HEX(f) from_ascii (f, sizeof f, LG_16)
uintmax_t
from_ascii (char const *where, size_t digs, unsigned logbase);
int ascii_to_hash(unsigned char *hash, const char *s);
void hash_to_ascii(const unsigned char *hash, char *s);
int IsValidHash(const unsigned char *hash);

#define max(a, b) ({\
		typeof(a) _a = a;\
		typeof(b) _b = b;\
		_a > _b ? _a : _b; })

#define min(a, b) ({\
		typeof(a) _a = a;\
		typeof(b) _b = b;\
		_a < _b ? _a : _b; })


int gpio_export(int gpio);
int gpio_unexport(int gpio);
int gpio_direction_input(int gpio_number);
int gpio_direction_output(int gpio_number, int value);
int gpio_set_value(int gpio_number, int value);
int gpio_get_value(int gpio_number);

int fill_buffer(int fd, unsigned char *buf, int nbytes, unsigned long *offs,
	uint32_t *checksum, void *dgst);
int decompress_image(int infile, unsigned long *offs, int nbytes,
	int outfile, uint32_t *checksum, void *dgst);
int fw_set_one_env(const char *name, const char *value);
int openfile(const char *filename);
int copyfile(int fdin, int fdout, int nbytes, unsigned long *offs,
	int skip_file, int compressed, uint32_t *checksum, unsigned char *hash);
int copyimage(int fdout, struct img_type *img);
off_t extract_sw_description(int fd, const char *descfile, off_t start);
off_t extract_next_file(int fd, int fdout, off_t start, int compressed,
	       			unsigned char *hash);
int openfileoutput(const char *filename);

int register_notifier(notifier client);
void notify(RECOVERY_STATUS status, int level, const char *msg);
void notify_init(void);
int syslog_init(void);

char **splitargs(char *args, int *argc);
void freeargs (char **argv);
int isDirectoryEmpty(const char *dirname);
int get_hw_revision(struct hw_type *hw);
void get_sw_versions(struct swupdate_cfg *sw);
int check_hw_compatibility(struct swupdate_cfg *cfg);
int count_elem_list(struct imglist *list);
#endif
