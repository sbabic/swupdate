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
#include <string.h>
#include "swupdate.h"
#include "swupdate_status.h"

#define NOTIFY_BUF_SIZE 	2048

extern int loglevel;

/*
 * loglevel is used into TRACE / ERROR
 * for values > LASTLOGLEVEL, it is an encoded field 
 * to inform the installer about a change in a subprocess
 */
typedef enum {
	OFF,
	ERRORLEVEL,
	WARNLEVEL,
	INFOLEVEL,
	DEBUGLEVEL,
	TRACELEVEL,
	LASTLOGLEVEL=TRACELEVEL
} LOGLEVEL;

/*
 * Following are used for notification from another process
 */

typedef enum {
	CANCELUPDATE=LASTLOGLEVEL + 1,
} NOTIFY_CAUSE;

enum {
	RECOVERY_NO_ERROR,
	RECOVERY_ERROR,
};

struct installer {
	int	fd;			/* install image file handle */
	RECOVERY_STATUS	status;		/* "idle" or "request source" info */
	RECOVERY_STATUS	last_install;	/* result from last installation */
	int	last_error;		/* error code if installation failed */
	char	errormsg[64];		/* error message if installation failed */
	sourcetype source; 		/* Who triggered the update */
	unsigned int len;    		/* Len of data valid in info */
	char	info[2048];   		/* info */
};

typedef void (*notifier) (RECOVERY_STATUS status, int level, const char *msg);

#define swupdate_notify(status, format, level, arg...) do { \
	if (loglevel >= level) { \
		char tmpbuf[NOTIFY_BUF_SIZE]; \
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

#ifndef typeof
#define typeof __typeof__
#endif
#define max(a, b) ({\
		typeof(a) _a = a;\
		typeof(b) _b = b;\
		_a > _b ? _a : _b; })

#define min(a, b) ({\
		typeof(a) _a = a;\
		typeof(b) _b = b;\
		_a < _b ? _a : _b; })

char *sdup(const char *str);

/*
 * Function to extract / copy images
 */
typedef int (*writeimage) (void *out, const void *buf, int len);

int fill_buffer(int fd, unsigned char *buf, unsigned int nbytes, unsigned long *offs,
	uint32_t *checksum, void *dgst);
int decompress_image(int infile, unsigned long *offs, int nbytes,
	int outfile, uint32_t *checksum, void *dgst);

int openfile(const char *filename);
int copyfile(int fdin, void *out, unsigned int nbytes, unsigned long *offs,
	unsigned long long seek,
	int skip_file, int compressed, uint32_t *checksum,
	unsigned char *hash, int encrypted, writeimage callback);
int copyimage(void *out, struct img_type *img, writeimage callback);
off_t extract_sw_description(int fd, const char *descfile, off_t start);
off_t extract_next_file(int fd, int fdout, off_t start, int compressed,
			int encrypted, unsigned char *hash);
int openfileoutput(const char *filename);

int register_notifier(notifier client);
void notify(RECOVERY_STATUS status, int level, const char *msg);
void notify_init(void);
int syslog_init(void);

char **splitargs(char *args, int *argc);
char** string_split(char* a_str, const char a_delim);
void freeargs (char **argv);
int isDirectoryEmpty(const char *dirname);
int get_hw_revision(struct hw_type *hw);
void get_sw_versions(char *cfgfname, struct swupdate_cfg *sw);
int check_hw_compatibility(struct swupdate_cfg *cfg);
int count_elem_list(struct imglist *list);

/* Decryption key functions */
int load_decryption_key(char *fname);
unsigned char *get_aes_key(void);
unsigned char *get_aes_ivt(void);

/* Getting global information */
int get_install_info(sourcetype *source, char *buf, size_t len);

unsigned long long ustrtoull(const char *cp, char **endp, unsigned int base);

#endif
