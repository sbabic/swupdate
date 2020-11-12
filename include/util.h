/*
 * (C) Copyright 2012-2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#ifndef _UTIL_H
#define _UTIL_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#if defined(__linux__)
#include <linux/types.h>
#endif
#include "swupdate.h"
#include "swupdate_status.h"
#include "compat.h"

#define NOTIFY_BUF_SIZE 	2048
#define ENOMEM_ASPRINTF		-1

#define SWUPDATE_SHA_DIGEST_LENGTH	20
#define AES_BLK_SIZE	16
#define AES_128_KEY_LEN	16
#define AES_192_KEY_LEN	24
#define AES_256_KEY_LEN	32

#define HWID_REGEXP_PREFIX	"#RE:"
#define SWUPDATE_ALIGN(A,S)    (((A) + (S) - 1) & ~((S) - 1))

extern int loglevel;

typedef enum {
	SERVER_OK,
	SERVER_EERR,
	SERVER_EBADMSG,
	SERVER_EINIT,
	SERVER_EACCES,
	SERVER_EAGAIN,
	SERVER_UPDATE_AVAILABLE,
	SERVER_NO_UPDATE_AVAILABLE,
	SERVER_UPDATE_CANCELED,
	SERVER_ID_REQUESTED,
} server_op_res_t;

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
	TRACELEVEL,
	DEBUGLEVEL,
	LASTLOGLEVEL=DEBUGLEVEL
} LOGLEVEL;

/*
 * Following are used for notification from another process
 */

typedef enum {
	CANCELUPDATE=LASTLOGLEVEL + 1,
	CHANGE,
} NOTIFY_CAUSE;

enum {
	RECOVERY_NO_ERROR,
	RECOVERY_ERROR,
	RECOVERY_DWL,
};

typedef void (*notifier) (RECOVERY_STATUS status, int error, int level, const char *msg);

void notify(RECOVERY_STATUS status, int error, int level, const char *msg);
void notify_init(void);
void notifier_set_color(int level, char *col);
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
			notify(FAILURE, 0, level, tmpbuf); \
		} else {\
			snprintf(tmpbuf, sizeof(tmpbuf), \
				       	"[%s] : " format, __func__, ## arg); \
			notify(RUN, RECOVERY_NO_ERROR, level, tmpbuf); \
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

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#define STRINGIFY(...) #__VA_ARGS__
#define SETSTRING(p, v) do { \
	if (p) \
		free(p); \
	p = strdup(v); \
} while (0)


#define LG_16 4
#define FROM_HEX(f) from_ascii (f, sizeof f, LG_16)
#if !defined(CONFIG_DISABLE_CPIO_CRC)
static inline bool swupdate_verify_chksum(const uint32_t chk1, const uint32_t chk2) {
	bool ret = (chk1 == chk2);
	if (!ret) {
		ERROR("Checksum WRONG ! Computed 0x%ux, it should be 0x%ux",
			chk1, chk2);
	}
	return ret;
}
#else
static inline bool swupdate_verify_chksum(
		const uint32_t  __attribute__ ((__unused__))chk1,
		const uint32_t  __attribute__ ((__unused__))chk2) {
	return true;
}
#endif
uintmax_t
from_ascii (char const *where, size_t digs, unsigned logbase);
int ascii_to_hash(unsigned char *hash, const char *s);
int ascii_to_bin(unsigned char *dest, size_t dstlen, const char *src);
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
typedef int (*writeimage) (void *out, const void *buf, unsigned int len);

int openfile(const char *filename);
int copy_write(void *out, const void *buf, unsigned int len);
#if defined(__FreeBSD__)
int copy_write_padded(void *out, const void *buf, unsigned int len);
#endif
#if defined(__linux__)
/* strlcpy was originally developped in FreeBSD, not present in glibc */
size_t
strlcpy(char *dst, const char * src, size_t size);
#endif
int copyfile(int fdin, void *out, unsigned int nbytes, unsigned long *offs,
	unsigned long long seek,
	int skip_file, int compressed, uint32_t *checksum,
	unsigned char *hash, int encrypted, const char *imgivt, writeimage callback);
int copyimage(void *out, struct img_type *img, writeimage callback);
int extract_sw_description(int fd, const char *descfile, off_t *offs);
off_t extract_next_file(int fd, int fdout, off_t start, int compressed,
			int encrypted, char *ivt, unsigned char *hash);
int openfileoutput(const char *filename);
int mkpath(char *dir, mode_t mode);
int swupdate_file_setnonblock(int fd, bool block);

int register_notifier(notifier client);
int syslog_init(void);

char **splitargs(char *args, int *argc);
char *mstrcat(const char **nodes, const char *delim);
char** string_split(const char* a_str, const char a_delim);
char *substring(const char *src, int first, int len);
size_t snescape(char *dst, size_t n, const char *src);
void freeargs (char **argv);
int get_hw_revision(struct hw_type *hw);
void get_sw_versions(char *cfgfname, struct swupdate_cfg *sw);
int compare_versions(const char* left_version, const char* right_version);
int hwid_match(const char* rev, const char* hwrev);
int check_hw_compatibility(struct swupdate_cfg *cfg);
int count_elem_list(struct imglist *list);
unsigned int count_string_array(const char **nodes);
void free_string_array(char **nodes);

/* Decryption key functions */
int load_decryption_key(char *fname);
unsigned char *get_aes_key(void);
char get_aes_keylen(void);
unsigned char *get_aes_ivt(void);
int set_aes_key(const char *key, const char *ivt);
int set_aes_ivt(const char *ivt);

/* Getting global information */
int get_install_info(sourcetype *source, char *buf, size_t len);
void get_install_swset(char *buf, size_t len);
void get_install_running_mode(char *buf, size_t len);

unsigned long long ustrtoull(const char *cp, unsigned int base);

const char* get_tmpdir(void);
const char* get_tmpdirscripts(void);

int swupdate_mount(const char *device, const char *dir, const char *fstype);
int swupdate_umount(const char *dir);

/* Date / Time utilities */
char *swupdate_time_iso8601(void);
#endif
