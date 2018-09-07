/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <dirent.h>
#include <limits.h>

#include "swupdate.h"
#include "util.h"
#include "generated/autoconf.h"

/*
 * key  is 256 bit for aes_256
 * ivt  is 128 bit
 * salt is  64 bit
 */
struct decryption_key {
	unsigned char key[32];
	unsigned char ivt[16];
	unsigned char salt[8];
};

static struct decryption_key *aes_key = NULL;

char *sdup(const char *str) {
	char *p;
	if ((p = (char *) malloc(strlen(str) + 1)) != NULL) {
		strcpy(p, str);
	}
	return p;
}

static char* TMPDIR = NULL;
static char* TMPDIRSCRIPT = NULL;

const char* get_tmpdir(void)
{
	if (TMPDIR != NULL) {
		return TMPDIR;
	}

	char *env_tmpdir = getenv("TMPDIR");
	if (env_tmpdir == NULL) {
		TMPDIR = (char*)"/tmp/";
		return TMPDIR;
	}

	if (env_tmpdir[strlen(env_tmpdir)] == '/') {
		TMPDIR = env_tmpdir;
		return TMPDIR;
	}

	if (asprintf(&TMPDIR, "%s/", env_tmpdir) == -1) {
		TMPDIR = (char*)"/tmp/";
	}
	return TMPDIR;
}

const char* get_tmpdirscripts(void)
{
	if (TMPDIRSCRIPT != NULL) {
		return TMPDIRSCRIPT;
	}

	if (asprintf(&TMPDIRSCRIPT, "%s%s", get_tmpdir(), SCRIPTS_DIR_SUFFIX) == -1) {
		TMPDIRSCRIPT = (char*)"/tmp/" SCRIPTS_DIR_SUFFIX;
	}
	return TMPDIRSCRIPT;
}

static int countargc(char *args, char **argv)
{
	int count = 0;

	while (isspace (*args))
		++args;
	while (*args) {
		if (argv)
			argv[count] = args;
		while (*args && !isspace (*args))
			++args;
		if (argv && *args)
			*args++ = '\0';
		while (isspace (*args))
			++args;
		count++;
	}
	return count;
}

char **splitargs(char *args, int *argc)
{
	char **argv = NULL;
	int argn = 0;

	if (args && *args
		&& (args = strdup(args))
		&& (argn = countargc(args, NULL))
		&& (argv = malloc((argn + 2) * sizeof (char *)))) {
			*argv++ = args;
			argn = countargc(args, argv);
			argv[argn] = NULL;
		}

		if (args && !argv)
			free (args);

		*argc = argn;
		return argv;
}

void freeargs (char **argv)
{
	if (argv) {
		free(argv[-1]);
		free(argv - 1);
	}
}

int openfileoutput(const char *filename)
{
	int fdout;

	fdout = open(filename, O_CREAT | O_WRONLY | O_TRUNC,  S_IRUSR | S_IWUSR );
	if (fdout < 0)
		ERROR("I cannot open %s %d", filename, errno);

	return fdout;
}

/*
 * This function is strict bounded with the hardware
 * It reads some GPIOs to get the hardware revision
 */
int get_hw_revision(struct hw_type *hw)
{
	FILE *fp;
	int ret;
	char *b1, *b2;
#ifdef CONFIG_HW_COMPATIBILITY_FILE
#define HW_FILE CONFIG_HW_COMPATIBILITY_FILE
#else
#define HW_FILE "/etc/hwrevision"
#endif

	if (!hw)
		return -EINVAL;

	/*
	 * do not overwrite if it is already set
	 * (maybe from command line)
	 */
	if (strlen(hw->boardname))
		return 0;

	memset(hw->boardname, 0, sizeof(hw->boardname));
	memset(hw->revision, 0, sizeof(hw->revision));

	/*
	 * Not all boards have pins for revision number
	 * check if there is a file containing theHW revision number
	 */
	fp = fopen(HW_FILE, "r");
	if (!fp)
		return -1;

	ret = fscanf(fp, "%ms %ms", &b1, &b2);
	fclose(fp);

	if (ret != 2) {
		TRACE("Cannot find Board Revision");
		if(ret == 1)
			free(b1);
		return -1;
	}

	if ((strlen(b1) > (SWUPDATE_GENERAL_STRING_SIZE) - 1) ||
		(strlen(b2) > (SWUPDATE_GENERAL_STRING_SIZE - 1))) {
		ERROR("Board name or revision too long");
		return -1;
	}

	strncpy(hw->boardname, b1, sizeof(hw->boardname));
	strncpy(hw->revision, b2, sizeof(hw->revision));

	free(b1);
	free(b2);

	return 0;
}

/*
 * The HW revision of the board *MUST* be inserted
 * in the sw-description file
 */
#ifdef CONFIG_HW_COMPATIBILITY
int check_hw_compatibility(struct swupdate_cfg *cfg)
{
	struct hw_type *hw;
	int ret;

	ret = get_hw_revision(&cfg->hw);
	if (ret < 0)
		return -1;

	TRACE("Hardware %s Revision: %s", cfg->hw.boardname, cfg->hw.revision);
	LIST_FOREACH(hw, &cfg->hardware, next) {
		if (hw && strlen(hw->revision) == strlen(cfg->hw.revision) &&
				(!strcmp(hw->revision, cfg->hw.revision))) {
			TRACE("Hardware compatibility verified");
			return 0;
		}
	}

	return -1;
}
#else
int check_hw_compatibility(struct swupdate_cfg
		__attribute__ ((__unused__)) *cfg)
{
	return 0;
}
#endif

uintmax_t
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
			ERROR("Malformed number %.*s", (int)digs, where);
			break;
		}

		d = p - codetab;
		if ((d >> logbase) > 1)
		{
			ERROR("Malformed number %.*s", (int)digs, where);
			break;
		}
		value += d;
		if (++buf == end || *buf == 0)
			break;
		overflow |= value ^ (value << logbase >> logbase);
		value <<= logbase;
	}
	if (overflow)
		ERROR("Archive value %.*s is out of range",
			(int)digs, where);
	return value;
}

/*
 * Convert a hash as hexa string into a sequence of bytes
 * hash must be an array of 32 bytes as specified by SHA256
 */
static int ascii_to_bin(unsigned char *hash, const char *s, size_t len)
{
	unsigned int i;
	unsigned int val;

	if (s == NULL) {
		return 0;
	}

	if (len % 2)
		return -EINVAL;
	if (strlen(s) == len) {
		for (i = 0; i < len; i+= 2) {
			val = from_ascii(&s[i], 2, LG_16);
			hash[i / 2] = val;
		}
	} else
		return -1;

	return 0;
}

int ascii_to_hash(unsigned char *hash, const char *s)
{
	return ascii_to_bin(hash, s, 64);
}

void hash_to_ascii(const unsigned char *hash, char *str)
{
	int i;
	char *s = str;

	for (i = 0; i < SHA256_HASH_LENGTH; i++) {
		sprintf(s, "%02x", hash[i]);
		s += 2;
	}
	*s = '\0';
}

/*
 * Check that hash is not zeroes
 */
int IsValidHash(const unsigned char *hash)
{
	int i;

	if (!hash)
		return 0;

	for (i = 0; i < SHA256_HASH_LENGTH; i++) {
		if (hash[i] != 0)
			return 1;		
	}

	return 0;
}

int count_elem_list(struct imglist *list)
{
	int count = 0;
	struct img_type *img;

	LIST_FOREACH(img, list, next) {
		count++;
	}

	return count;
}

int load_decryption_key(char *fname)
{
	FILE *fp;
	char *b1 = NULL, *b2 = NULL, *b3 = NULL;
	int ret;

	fp = fopen(fname, "r");
	if (!fp)
		return -EBADF;

	ret = fscanf(fp, "%ms %ms %ms", &b1, &b2, &b3);
	switch (ret) {
		case 2:
			b3 = NULL;
			DEBUG("Read decryption key and initialization vector from file %s.", fname);
			break;
		case 3:
			DEBUG("Read decryption key, initialization vector, and salt from file %s.", fname);
			break;
		default:
			if (b1 != NULL)
				free(b1);
			fprintf(stderr, "File with decryption key is not in the format <key> <ivt> nor <key> <ivt> <salt>\n");
			fclose(fp);
			return -EINVAL;
	}
	fclose(fp);

	if (aes_key)
		free(aes_key);

	aes_key = (struct decryption_key *)calloc(1, sizeof(*aes_key));
	if (!aes_key)
		return -ENOMEM;

	ret = ascii_to_bin(aes_key->key,  b1, sizeof(aes_key->key)  * 2) |
	      ascii_to_bin(aes_key->ivt,  b2, sizeof(aes_key->ivt)  * 2) |
	      ascii_to_bin(aes_key->salt, b3, sizeof(aes_key->salt) * 2);

	if (b1 != NULL)
		free(b1);
	if (b2 != NULL)
		free(b2);
	if (b3 != NULL)
		free(b3);

	if (ret) {
		fprintf(stderr, "Keys are invalid\n");
		return -EINVAL;
	}

	return 0;
}

unsigned char *get_aes_key(void) {
	if (!aes_key)
		return NULL;
	return aes_key->key;
}

unsigned char *get_aes_ivt(void) {
	if (!aes_key)
		return NULL;
	return aes_key->ivt;
}

unsigned char *get_aes_salt(void) {
	if (!aes_key)
		return NULL;
	return aes_key->salt;
}

char** string_split(const char* in, const char d)
{
	char** result    = 0;
	size_t count     = 0;
	char* last_delim = 0;
	char delim[2];
	delim[0] = d;
	delim[1] = 0;
	char *s = strdup(in);
	char* tmp        = s;
	if (!s)
		return NULL;

	/* Count how many elements will be extracted. */
	while (*tmp)
	{
	    if (d == *tmp)
	    {
	        count++;
	        last_delim = tmp;
	    }
	    tmp++;
	}

	/* Add space for trailing token. */
	count += last_delim < (s + strlen(s) - 1);

	/* Add space for terminating null string so caller
	   knows where the list of returned strings ends. */
	count++;

	result = malloc(sizeof(char*) * count);

	if (result)
	{
	    size_t idx  = 0;
	    char* token = strtok(s, delim);

	    while (token)
	    {
	        *(result + idx++) = strdup(token);
	        token = strtok(0, delim);
	    }
	    *(result + idx) = 0;
	}

	free(s);

	return result;
}

unsigned long long ustrtoull(const char *cp, unsigned int base)
{
	errno = 0;
	char *endp = NULL;

	if (strnlen(cp, MAX_SEEK_STRING_SIZE) == 0) {
		return 0;
	}

	unsigned long long result = strtoull(cp, &endp, base);

	if (cp == endp || (result == ULLONG_MAX && errno == ERANGE)) {
		errno = ERANGE;
		return 0;
	}

	switch (*endp) {
	case 'G':
		result *= 1024;
		/* fall through */
	case 'M':
		result *= 1024;
		/* fall through */
	case 'K':
	case 'k':
		result *= 1024;
		if (endp[1] == 'i') {
			if (endp[2] == 'B')
				endp += 3;
			else
				endp += 2;
		}
	}
	return result;
}

int swupdate_mount(const char *device, const char *dir, const char *fstype)
{
#if defined(__linux__)
	return mount(device, dir, fstype, 0, NULL);
#elif defined(__FreeBSD__)
	int iovlen = 8;
	struct iovec iov[iovlen];
	int mntflags = 0;
	char errmsg[255];
	memset(errmsg, 0, sizeof(errmsg));
	iov[0].iov_base = (void*)"fstype";
	iov[0].iov_len = strlen("fstype") + 1;
	iov[1].iov_base = (void*)fstype;
	iov[1].iov_len = strlen(fstype) + 1;
	iov[2].iov_base = (void*)"fspath";
	iov[2].iov_len = strlen("fspath") + 1;
	iov[3].iov_base = (void*)dir;
	iov[3].iov_len = strlen(dir) + 1;
	iov[4].iov_base = (void*)"from";
	iov[4].iov_len = strlen("from") + 1;
	iov[5].iov_base = (void*)device;
	iov[5].iov_len = strlen(device) + 1;
	/* The underlying fs driver may require a
	   buffer for an error message, even if we
	   do not use it here. */
	iov[6].iov_base = (void*)"errmsg";
	iov[6].iov_len = strlen("errmsg") + 1;
	iov[7].iov_base = errmsg;
	iov[7].iov_len = strlen(errmsg) + 1;
	return nmount(iov, iovlen, mntflags);
#else
	/* Not implemented for this OS, no specific errno. */
	errno = 0;
	return -1;
#endif
}

int swupdate_umount(const char *dir)
{
#if defined(__linux__)
	return umount(dir);
#elif defined(__FreeBSD__)
	int mntflags = 0;
	return unmount(dir, mntflags);
#else
	/* Not implemented for this OS, no specific errno. */
	errno = 0;
	return -1;
#endif
}

