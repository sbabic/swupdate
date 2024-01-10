/*
 * (C) Copyright 2012-2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/time.h>
#if defined(__linux__)
#include <linux/types.h>
#endif
#include <sys/types.h>
#include "globals.h"
#include "swupdate_status.h"
#include "swupdate_dict.h"
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

#define BOOTVAR_TRANSACTION "recovery_status"

struct img_type;
struct imglist;
struct hw_type;

extern int loglevel;
extern int exit_code;

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

enum {
  COMPRESSED_FALSE,
  COMPRESSED_TRUE,
  COMPRESSED_ZLIB,
  COMPRESSED_ZSTD,
};

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

#define __FILENAME__ (__builtin_strrchr(__FILE__, '/') ? __builtin_strrchr(__FILE__, '/') + 1 : __FILE__)
#define swupdate_notify(status, format, level, arg...) do { \
	if (loglevel >= level) { \
		char tmpbuf[NOTIFY_BUF_SIZE]; \
		if (status == FAILURE) { \
			if (loglevel >= DEBUGLEVEL) \
				snprintf(tmpbuf, sizeof(tmpbuf), \
				     	"ERROR %s : %s : %d : " format, \
						__FILENAME__, \
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
#define PREPROCVALUE(s) STRINGIFY(s)
#define SETSTRING(p, v) do { \
	if (p) \
		free(p); \
	p = strdup(v); \
} while (0)


#define IS_STR_EQUAL(s,s1) (s && s1 && !strcmp(s,s1))
#define UNUSED __attribute__((__unused__))

#define LG_16 4
#define FROM_HEX(f) from_ascii (f, sizeof f, LG_16)
uintmax_t
from_ascii (char const *where, size_t digs, unsigned logbase);
int ascii_to_hash(unsigned char *hash, const char *s);
int ascii_to_bin(unsigned char *dest, size_t dstlen, const char *src);
void hash_to_ascii(const unsigned char *hash, char *s);
int IsValidHash(const unsigned char *hash);
bool is_hex_str(const char *ascii);

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

#define min_t(type,x,y) \
	({ type __x = (x); type __y = (y); __x < __y ? __x: __y; })
#define max_t(type,x,y) \
	({ type __x = (x); type __y = (y); __x > __y ? __x: __y; })

char *sdup(const char *str);
bool strtobool(const char *s);

/*
 * Function to extract / copy images
 */
typedef int (*writeimage) (void *out, const void *buf, size_t len);

void *saferealloc(void *ptr, size_t size);
int copy_write(void *out, const void *buf, size_t len);
#if defined(__FreeBSD__)
int copy_write_padded(void *out, const void *buf, size_t len);
#endif
#if defined(__linux__)
/* strlcpy was originally developped in FreeBSD, not present in glibc */
size_t
strlcpy(char *dst, const char * src, size_t size);
#endif
int copyfile(int fdin, void *out, size_t nbytes, unsigned long *offs,
	unsigned long long seek,
	int skip_file, int compressed, uint32_t *checksum,
	unsigned char *hash, bool encrypted, const char *imgivt, writeimage callback);
int copyimage(void *out, struct img_type *img, writeimage callback);
int copybuffer(unsigned char *inbuf, void *out, size_t nbytes, int compressed,
	unsigned char *hash, bool encrypted, const char *imgivt, writeimage callback);
int openfileoutput(const char *filename);
int mkpath(char *dir, mode_t mode);
int swupdate_file_setnonblock(int fd, bool block);

int register_notifier(notifier client);
int syslog_init(void);

char **splitargs(char *args, int *argc);
char *mstrcat(const char **nodes, const char *delim);
char** string_split(const char* a_str, const char a_delim);
char *substring(const char *src, int first, int len);
char *string_tolower(char *s);
size_t snescape(char *dst, size_t n, const char *src);
void freeargs (char **argv);
int compare_versions(const char* left_version, const char* right_version);
int hwid_match(const char* rev, const char* hwrev);
int count_elem_list(struct imglist *list);
unsigned int count_string_array(const char **nodes);
void free_string_array(char **nodes);
int read_lines_notify(int fd, char *buf, int buf_size, int *buf_offset,
		      LOGLEVEL level);
long long get_output_size(struct img_type *img, bool strict);
bool img_check_free_space(struct img_type *img, int fd);
bool check_same_file(int fd1, int fd2);

/* location for libubootenv configuration file */
const char *get_fwenv_config(void);
void set_fwenv_config(const char *fname);

/* Decryption key functions */
int load_decryption_key(char *fname);
unsigned char *get_aes_key(void);
char get_aes_keylen(void);
unsigned char *get_aes_ivt(void);
int set_aes_key(const char *key, const char *ivt);

/* Getting global information */
int get_install_info(char *buf, size_t len);
sourcetype  get_install_source(void);
void get_install_swset(char *buf, size_t len);
void get_install_running_mode(char *buf, size_t len);
char *get_root_device(void);

/* Setting global information */
void set_version_range(const char *minversion,
			const char *maxversion,
			const char *current);

int size_delimiter_match(const char *size);
unsigned long long ustrtoull(const char *cp, char **endptr, unsigned int base);

const char* get_tmpdir(void);
const char* get_tmpdirscripts(void);

void swupdate_create_directory(const char* path);
#ifndef CONFIG_NOCLEANUP
int swupdate_remove_directory(const char* path);
#endif

int swupdate_mount(const char *device, const char *dir, const char *fstype);
int swupdate_umount(const char *dir);

/* Date / Time utilities */
char *swupdate_time_iso8601(struct timeval *tv);

/* eMMC functions */
int emmc_write_bootpart(int fd, int bootpart);
int emmc_get_active_bootpart(int fd);
