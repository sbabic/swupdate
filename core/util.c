/*
 * (C) Copyright 2013
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <dirent.h>
#include <limits.h>
#include <time.h>
#include <libgen.h>
#include <regex.h>
#include <string.h>
#include <dirent.h>
#include "swupdate_dict.h"
#include "swupdate_image.h"

#if defined(__linux__)
#include <sys/statvfs.h>
#endif

#include "util.h"
#include "generated/autoconf.h"

/*
 * key    is 256 bit for max aes_256
 * keylen is the actual aes key length
 * ivt    is 128 bit
 */
struct decryption_key {
#ifdef CONFIG_PKCS11
	char * key;
#else
	unsigned char key[AES_256_KEY_LEN];
#endif
	char keylen;
	unsigned char ivt[AES_BLK_SIZE];
};

static struct decryption_key *aes_key = NULL;

/*
 * Configuration file for fw_env.config
 */

static char *fwenv_config = NULL;

char *sdup(const char *str) {
	char *p;
	if ((p = (char *) malloc(strlen(str) + 1)) != NULL) {
		strcpy(p, str);
	}
	return p;
}

static char* TMPDIR = NULL;
static char* TMPDIRSCRIPT = NULL;

/*
 * Convert a hash as hexa string into a sequence of bytes
 * hash must be an array of 32 bytes as specified by SHA256
 */
int ascii_to_bin(unsigned char *dest, size_t dstlen, const char *src)
{
	unsigned int i;
	unsigned int val;
	size_t srclen;

	if (src == NULL) {
		return 0;
	}

	srclen = strlen(src);

	if (srclen % 2)
		return -EINVAL;
	if (srclen == 2 * dstlen) {
		for (i = 0; i < dstlen; i++) {
			val = from_ascii(&src[i*2], 2, LG_16);
			dest[i] = val;
		}
	} else
		return -EINVAL;

	return 0;
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

void swupdate_create_directory(const char* path) {
	char* dpath;
	if (asprintf(&dpath, "%s%s", get_tmpdir(), path) ==
		ENOMEM_ASPRINTF) {
		ERROR("OOM: Directory %s not created", path);
		return;
	}
	if (mkdir(dpath, 0777)) {
		WARN("Directory %s cannot be created: %s",
			 dpath, strerror(errno));
	}
	free(dpath);
}

#ifndef CONFIG_NOCLEANUP
static int _remove_directory_cb(const char *fpath, const struct stat *sb,
								int typeflag, struct FTW *ftwbuf)
{
	(void)sb;
	(void)typeflag;
	(void)ftwbuf;
	return remove(fpath);
}

int swupdate_remove_directory(const char* path)
{
	char* dpath;
	int ret;
	if (asprintf(&dpath, "%s%s", get_tmpdir(), path) ==
		ENOMEM_ASPRINTF) {
		ERROR("OOM: Directory %s not removed", path);
		return -ENOMEM;
	}
	ret = nftw(dpath, _remove_directory_cb, 64, FTW_DEPTH | FTW_PHYS);
	free(dpath);
	return ret;
}
#endif

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

void *saferealloc(void *ptr, size_t size) {
    void *ret = realloc(ptr, size);
    /*
     * Realloc does not touch the original block if fails.
     * Policy is to free memory and returns with error (Null)
     */
    if (!ret && ptr)
        free(ptr);
    return ret;
}


/*
 * Concatente array of strings in a single string
 * The allocated string must be freed by the caller
 * delim can be used to separate substrings
 */
char *mstrcat(const char **nodes, const char *delim)
{
	const char **node;
	char *dest = NULL, *buf = NULL;

	if (!delim)
		delim = "";
	for (node = nodes; *node != NULL; node++) {
		/* first run, just copy first entry */
		if (!dest) {
			dest = strdup(*node);
			if (!dest)
				return NULL;
		} else {
			if (asprintf(&buf, "%s%s%s", dest, delim, *node) ==
				ENOMEM_ASPRINTF) {
				ERROR("Path too long, OOM");
				free(dest);
				return NULL;
			}

			/*
			 * Free previous concatenated string
			 */
			free(dest);
			dest = buf;
		}
	}
	return dest;
}

/*
 * Alocate and return a string as part of
 * another string
 * s = substring(src, n)
 * the returned string is allocated on the heap
 * and must be freed by the caller
 */
char *substring(const char *src, int first, int len) {
	char *s;
	if (len > strlen(src))
		len = strlen(src);
	if (first > len)
		return NULL;
	s = malloc(len + 1);
	if (!s)
		return NULL;
	memcpy(s, &src[first], len);
	s[len] = '\0';
	return s;
}

/*
 * Convert all chars of a string to lower,
 * there is no ready to use function
 */

char *string_tolower(char *s)
{
	char *p = s;
	for ( ; *p; ++p) *p = tolower(*p);
	return s;
}

int openfileoutput(const char *filename)
{
	int fdout;

	fdout = open(filename, O_CREAT | O_WRONLY | O_TRUNC,  S_IRUSR | S_IWUSR );
	if (fdout < 0)
		ERROR("I cannot open %s %d", filename, errno);

	return fdout;
}

int mkpath(char *dir, mode_t mode)
{
	if (!dir) {
		return -EINVAL;
	}

	if (strlen(dir) == 1 && dir[0] == '/')
		return 0;

	(void) mkpath(dirname(strdupa(dir)), mode);

	if (mkdir(dir, mode) == -1) {
		if (errno != EEXIST)
			return 1;
	}
	return 0;
}

/**
 * hwid_match - try to match a literal or RE hwid
 * @rev: literal or RE specification
 * @hwrev: current HW revision
 *
 * Return: 0 if match found, non-zero otherwise
 */
int hwid_match(const char* rev, const char* hwrev)
{
	int ret, prefix_len;
	char errbuf[256];
	regex_t re;
	const char *re_str;

	prefix_len = strlen(HWID_REGEXP_PREFIX);

	/* literal revision */
	if (strncmp(rev, HWID_REGEXP_PREFIX, prefix_len)) {
		ret = strcmp(rev, hwrev);
		goto out;
	}

	/* regexp revision */
	re_str = rev+prefix_len;

	if ((ret = regcomp(&re, re_str, REG_EXTENDED|REG_NOSUB)) != 0) {
		regerror(ret, &re, errbuf, sizeof(errbuf));
		ERROR("error in regexp %s: %s", re_str, errbuf);
		goto out;
	}

	if ((ret = regexec(&re, hwrev, 0, NULL, 0)) == 0)
		TRACE("hwrev %s matched by regexp %s", hwrev, re_str);
	else
		TRACE("no match of hwrev %s with regexp %s", hwrev, re_str);

	regfree(&re);
out:
	return ret;
}

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

int ascii_to_hash(unsigned char *hash, const char *s)
{
	return ascii_to_bin(hash, SHA256_HASH_LENGTH, s);
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
	char *b1 = NULL, *b2 = NULL;
	int ret;

	fp = fopen(fname, "r");
	if (!fp)
		return -EBADF;

	ret = fscanf(fp, "%ms %ms", &b1, &b2);
	switch (ret) {
		case 2:
			DEBUG("Read decryption key and initialization vector from file %s.", fname);
			break;
		default:
			if (b1 != NULL)
				free(b1);
			fprintf(stderr, "File with decryption key is not in the format <key> <ivt>\n");
			fclose(fp);
			return -EINVAL;
	}
	fclose(fp);

	ret = set_aes_key(b1, b2);

	if (b1 != NULL)
		free(b1);
	if (b2 != NULL)
		free(b2);

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

char get_aes_keylen(void) {
	if (!aes_key)
		return -1;
	return aes_key->keylen;
}

unsigned char *get_aes_ivt(void) {
	if (!aes_key)
		return NULL;
	return aes_key->ivt;
}

bool is_hex_str(const char *ascii) {
	unsigned int i, size;

	if (!ascii)
		return false;

	size = strlen(ascii);
	if (!size)
		return false;

	for (i = 0;  i < size; ++i) {
		if (!isxdigit(ascii[i]))
			return false;
	}
	return true;
}

int set_aes_key(const char *key, const char *ivt)
{
	int ret;
	size_t keylen;

	/*
	 * Allocates the global structure just once
	 */
	if (!aes_key) {
		aes_key = (struct decryption_key *)calloc(1, sizeof(*aes_key));
		if (!aes_key)
			return -ENOMEM;
	}

	if (strlen(ivt) != (AES_BLK_SIZE*2) || !is_hex_str(ivt)) {
		ERROR("Invalid ivt");
		return -EINVAL;
	}

	ret = ascii_to_bin(aes_key->ivt, sizeof(aes_key->ivt), ivt);
#ifdef CONFIG_PKCS11
	keylen = strlen(key) + 1;
	aes_key->key = malloc(keylen);
	if (!aes_key->key)
		return -ENOMEM;
	strncpy(aes_key->key, key, keylen);
#else
	keylen = strlen(key);
	switch (keylen) {
	case AES_128_KEY_LEN * 2:
	case AES_192_KEY_LEN * 2:
	case AES_256_KEY_LEN * 2:
		// valid hex string size for AES 128/192/256
		aes_key->keylen = keylen / 2;
		break;
	default:
		ERROR("Invalid aes_key length");
		return -EINVAL;
	}
	ret |= !is_hex_str(key);
	ret |= ascii_to_bin(aes_key->key, aes_key->keylen, key);
#endif

	if (ret) {
		ERROR("Invalid aes_key");
		return -EINVAL;
	}

	return 0;
}

const char *get_fwenv_config(void) {
	if (!fwenv_config)
#if defined(CONFIG_UBOOT)
		return CONFIG_UBOOT_FWENV;
#else
		return NULL;
#endif
	return fwenv_config;
}

void set_fwenv_config(const char *fname) {
	if (!fname)
		return;

	if (fwenv_config)
		free(fwenv_config);

	fwenv_config = strdup(fname);
}


char** string_split(const char* in, const char d)
{
	char** result = 0;
	size_t count = 0;
	char* last_delim = 0;
	char delim[2];
	delim[0] = d;
	delim[1] = 0;
	char *s = strdup(in);
	char* tmp = s;
	if (!s)
		return NULL;

	/* Count how many elements will be extracted. */
	while (*tmp) {
		if (d == *tmp) {
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

	if (result) {
		size_t idx  = 0;
		char* token = strtok(s, delim);

		while (token) {
			*(result + idx++) = strdup(token);
			token = strtok(0, delim);
		}
		*(result + idx) = 0;
	}

	free(s);

	return result;
}

/*
 * Count number of elements in an array of strings
 * Last item must have a NULL terminator
 */
unsigned int count_string_array(const char **nodes)
{
	const char **iter = nodes;
	int count = 0;

	while (*iter != NULL) {
		iter++;
		count++;
	}
	return count;
}

void free_string_array(char **nodes)
{
	char **iter;
	if (!nodes)
		return;
	for (iter = nodes; *iter != NULL; iter++)
		free(*iter);
	free(nodes);
}

/*
 * Determines if ustrtoull() consumes a size-type delimiter
 * from the provided string.
 */
int size_delimiter_match(const char *size)
{
	char *suffix = NULL, *usuffix = NULL;
	strtoull(size, &suffix, 10);
	ustrtoull(size, &usuffix, 10);
	return suffix != usuffix;
}

/*
 * Like strtoull(), but automatically scales the conversion
 * result by size-type units, and only returns a pointer to
 * the size unit in the string if requested by the caller.
 *
 * Sets errno to ERANGE if strtoull() found no digits or
 * encountered an overflow, and returns 0 in both cases.
 */
unsigned long long ustrtoull(const char *cp, char **endptr, unsigned int base)
{
	errno = 0;
	char *endp = NULL;

	if (strnlen(cp, MAX_SEEK_STRING_SIZE) == 0) {
		return 0;
	}

	unsigned long long result = strtoull(cp, &endp, base);

	if (cp == endp || (result == ULLONG_MAX && errno == ERANGE)) {
		errno = ERANGE;
		result = 0;
		goto out;
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
		} else {
			endp += 1;
		}
	}

out:
	if (endptr)
		*endptr = endp;

	return result;
}

bool strtobool(const char *s)
{
	if (s && (!strcmp(s, "true") || !strcmp(s, "TRUE")))
		return true;
	return false;
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
	(void)device;
	(void)dir;
	(void)fstype;
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
	(void)dir;
	errno = 0;
	return -1;
#endif
}

/*
 * Date time in SWUpdate
 * @return : date in ISO8601 (it must be freed by caller)
 */
char *swupdate_time_iso8601(struct timeval *tv)
{
	#define DATE_SIZE_ISO8601	128
	struct timeval now;
	int ms;
	char msbuf[4];
	/* date length is fix, reserve enough space */
	char *buf = (char *)malloc(DATE_SIZE_ISO8601);
	char *tmp;

	if (!buf)
		return NULL;

	if (tv == NULL)
		gettimeofday(&now, NULL);
	else {
		now.tv_sec = tv->tv_sec;
		now.tv_usec = tv->tv_usec;
	}
	ms = now.tv_usec / 1000;

	(void)strftime(buf, DATE_SIZE_ISO8601, "%Y-%m-%dT%T.***%z", localtime(&now.tv_sec));

	/*
	 * Replace '*' placeholder with ms value
	 */
	snprintf(msbuf, sizeof(msbuf), "%03d", ms);
	tmp = strchr(buf, '*');
	if (tmp)
		memcpy(tmp, msbuf, 3);

	/*
	 * strftime add 4 bytes for timezone, ISO8601 uses
	 * +hh notation. Drop the last two bytes.
	 */
	buf[strlen(buf) - 2] = '\0';

	return buf;
}

int swupdate_file_setnonblock(int fd, bool block)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		return -EFAULT;

	if (block)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;

	return fcntl(fd, F_SETFL, flags);
}

/* Write escaped output to sized buffer */
size_t snescape(char *dst, size_t n, const char *src)
{
	size_t len = 0;

	if (n < 3)
		return 0;

	memset(dst, 0, n);

	for (int i = 0; src[i] != '\0'; i++) {
		if (src[i] == '\\' || src[i] == '\"') {
			if (len < n - 2)
				dst[len] = '\\';
			len++;
		}
		if (len < n - 1)
			dst[len] = src[i];
		len++;
	}

	return len;
}

/*
 * If major:minor is a containerized filesystem, e.g., in case of LUKS,
 * return the pointed-to device name.
 */
static int filter_slave(const struct dirent *ent) {
	return (strcmp(ent->d_name, ".") && strcmp(ent->d_name, ".."));
}
static char *get_root_containerized_fs(int major, int minor)
{
	struct dirent **devlist = NULL;
	char *devname = NULL;
	char buf[23+4+7+1];
	int ret = snprintf(buf, sizeof(buf), "/sys/dev/block/%d:%d/slaves", major, minor);
	if (ret < 0 || ret >= sizeof(buf)) {
		return NULL;
	}
	if (scandir(buf, &devlist, filter_slave, NULL) == 1) {
		devname = strdup(devlist[0]->d_name);
	}
	free(devlist);
	return devname;
}

/*
 * Return the real full path to a device, or NULL.
 */
static char *getroot_abs_path(char* devname)
{
	int fd;
	char *path;

	if (!devname)
		return NULL;

	if ((path = realpath(devname, NULL))) {
		if ((fd = open(path, O_RDWR | O_CLOEXEC)) != -1) {
			(void)close(fd);
			free(devname);
			return path;
		}
		free(path);
	}

	char* prefix = (char*)"/dev/";
	int prefix_len = strlen(prefix);
	int devname_len = strnlen(devname, PATH_MAX - prefix_len - 1);
	char *tmp = alloca(sizeof(char) * (prefix_len + devname_len + 1));
	(void)strcpy(strcpy(tmp, prefix) + prefix_len, devname);
	free(devname);
	if ((path = realpath(tmp, NULL))) {
		if ((fd = open(path, O_RDWR | O_CLOEXEC)) != -1) {
			(void)close(fd);
			return path;
		}
		free(path);
	}
	return NULL;
}

/*
 * Return the rootfs's device name from /proc/partitions supporting
 * containerized filesystems such as, e.g., LUKS.
 */
static char *get_root_from_partitions(void)
{
	struct stat info;
	FILE *fp;
	char *devname = NULL;
	unsigned long major, minor, nblocks;
	char buf[256];
	int ret, dev_major, dev_minor;

	if (stat("/", &info) < 0)
		return NULL;

	dev_major = info.st_dev / 256;
	dev_minor = info.st_dev % 256;

	devname = get_root_containerized_fs(dev_major, dev_minor);
	if (devname) {
		return getroot_abs_path(devname);
	}

	fp = fopen("/proc/partitions", "r");
	if (!fp)
		return NULL;

	while (fgets(buf, sizeof(buf), fp)) {
		ret = sscanf(buf, "%ld %ld %ld %ms",
			     &major, &minor, &nblocks, &devname);
		if (ret != 4)
			continue;
		if ((major == dev_major) && (minor == dev_minor)) {
			fclose(fp);
			return getroot_abs_path(devname);
		}
		free(devname);
	}

	fclose(fp);
	return NULL;
}

/*
 * Return the rootfs's device name from /proc/self/mountinfo.
 * Needed for filesystems having synthetic stat(2) st_dev
 * values such as BTRFS.
 */
static char *get_root_from_mountinfo(void)
{
	char *mnt_point, *device = NULL;
	unsigned int dev_major, dev_minor;
	FILE *fp = fopen("/proc/self/mountinfo", "r");
	if (!fp)
		return NULL;
	while (fp && !feof(fp)) {
		/* format: https://www.kernel.org/doc/Documentation/filesystems/proc.txt */
		if (fscanf(fp, "%*s %*s %u:%u %*s %ms %*s %*[-] %*s %ms %*s",
			   &dev_major, &dev_minor, &mnt_point, &device) == 4) {
			if ( (!strcmp(mnt_point, "/")) && (strcmp(device, "none")) ) {
				free(mnt_point);
				char *dpath;
				if ((dpath = realpath(device, NULL))) {
					free(device);
					device = dpath;
					struct stat dinfo;
					if (stat(device, &dinfo) == 0) {
						dev_major = dinfo.st_rdev / 256;
						dev_minor = dinfo.st_rdev % 256;
					}
				}
				if ((dpath = get_root_containerized_fs(dev_major, dev_minor))) {
					free(device);
					device = getroot_abs_path(dpath);
				}
				break;
			}
			free(mnt_point);
			free(device);
		}
		device = NULL;
	}
	(void)fclose(fp);
	return device;
}

#define MAX_CMDLINE_LENGTH 4096
static char *get_root_from_cmdline(void)
{
	char *buf;
	FILE *fp;
	char *root = NULL;
	int ret;
	char **parms = NULL;

	fp = fopen("/proc/cmdline", "r");
	if (!fp)
		return NULL;
	buf = (char *)calloc(1, MAX_CMDLINE_LENGTH);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	/*
	 * buf must be zero terminated, so let the last one
	 * for the NULL termination
	 */
	ret = fread(buf, 1, MAX_CMDLINE_LENGTH - 1, fp);

	/*
	 * this is just to drop coverity issue, but
	 * the buffer is already initialized by calloc to zeroes
	 */
	buf[MAX_CMDLINE_LENGTH - 1] = '\0';

	if (ret > 0) {
		parms = string_split(buf, ' ');
		if (!parms)
			goto out;
		int nparms = count_string_array((const char **)parms);
		for (unsigned int index = 0; index < nparms; index++) {
			if (!strncmp(parms[index], "root=", strlen("root="))) {
				const char *value = parms[index] + strlen("root=");
				root = getroot_abs_path(strdup(value));
				if (!root) {
					root = strdup(value);
				}
				break;
			}
		}
	}
out:
	fclose(fp);
	free_string_array(parms);
	free(buf);
	return root;
}

char *get_root_device(void)
{
	char *root = NULL;

	root = get_root_from_partitions();
	if (!root)
		root = get_root_from_mountinfo();
	if (!root)
		root = get_root_from_cmdline();

	return root;
}

int read_lines_notify(int fd, char *buf, int buf_size, int *buf_offset,
		      LOGLEVEL level)
{
	int n;
	int offset = *buf_offset;
	int print_last = 0;

	n = read(fd, &buf[offset], buf_size - offset - 1);
	if (n <= 0)
		return -errno;

	/* replace zeroes with @ signs */
	for (unsigned int index = 0; index < n; index++) {
		if (!buf[offset+index])
			buf[offset+index] = '@';
	}

	n += offset;
	buf[n] = '\0';

	/*
	 * Only print the last line of the split array if it represents a
	 * full line, as string_split (strtok) makes it impossible to tell
	 * afterwards if the buffer ended with separator.
	 */
	if (buf[n-1] == '\n') {
		print_last = 1;
	}

	char **lines = string_split(buf, '\n');
	if (!lines)
		return -errno;
	int nlines = count_string_array((const char **)lines);
	/*
	 * If the buffer is full and there is only one line,
	 * print it anyway.
	 */
	if (n >= buf_size - 1 && nlines == 1)
		print_last = 1;

	/* copy leftover data for next call */
	if (!print_last) {
		int left = snprintf(buf, buf_size, "%s", lines[nlines-1]);
		*buf_offset = left;
		nlines--;
		n -= left;
	} else { /* no need to copy, reuse all buffer */
		*buf_offset = 0;
	}

	for (unsigned int index = 0; index < nlines; index++) {
		RECOVERY_STATUS status = level == ERRORLEVEL ? FAILURE : RUN;
		swupdate_notify(status, "%s", level, lines[index]);
	}

	free_string_array(lines);

	return n;
}

long long get_output_size(struct img_type *img, bool strict)
{
	char *output_size_str = NULL;
	long long bytes = img->size;

	if (img->compressed) {
		output_size_str = dict_get_value(&img->properties, "decompressed-size");
		if (!output_size_str) {
			if (!strict)
				return bytes;

			ERROR("image is compressed but 'decompressed-size' property was not found");
			return -ENOENT;
		}

		bytes = ustrtoull(output_size_str, NULL, 0);
		if (errno || bytes <= 0) {
			ERROR("decompressed-size argument %s: ustrtoull failed",
			      output_size_str);
			return -1;
		}

		TRACE("Image is compressed, decompressed size %lld bytes", bytes);

	} else if (img->is_encrypted) {
		output_size_str = dict_get_value(&img->properties, "decrypted-size");
		if (!output_size_str) {
			if (!strict)
				return bytes;
			ERROR("image is encrypted but 'decrypted-size' property was not found");
			return -ENOENT;
		}

		bytes = ustrtoull(output_size_str, NULL, 0);
		if (errno || bytes <= 0) {
			ERROR("decrypted-size argument %s: ustrtoull failed",
			      output_size_str);
			return -1;
		}

		TRACE("Image is crypted, decrypted size %lld bytes", bytes);
	}

	return bytes;
}

static bool check_free_space(int fd, long long size, char *fname)
{
	/* This needs OS-specific implementation because linux's statfs
	 * f_bsize is optimal IO size vs. statvfs f_bsize fs block size,
	 * and freeBSD is the opposite...
	 * As everything else is the same down to field names work around
	 * this by just defining an alias
	 */
#if defined(__FreeBSD__)
#define statvfs statfs
#define fstatvfs fstatfs
#endif
	struct statvfs statvfs;
	unsigned long long free_space;

	if (fstatvfs(fd, &statvfs)) {
		ERROR("Statfs failed on %s, skipping free space check", fname);
		return true;
	}
	free_space = (unsigned long long)statvfs.f_bfree * statvfs.f_bsize;

	if (free_space < size) {
		ERROR("Not enough free space to extract %s (needed %llu, got %llu)",
		       fname, size, free_space);
		return false;
	}

	return true;
}

bool img_check_free_space(struct img_type *img, int fd)
{
	long long size;

	size = get_output_size(img, false);

	if (size <= 0)
		/* Skip check if no size found */
		return true;

	return check_free_space(fd, size, img->fname);
}

bool check_same_file(int fd1, int fd2) {
    struct stat stat1, stat2;
    if(fstat(fd1, &stat1) < 0) return false;
    if(fstat(fd2, &stat2) < 0) return false;
    return (stat1.st_dev == stat2.st_dev) && (stat1.st_ino == stat2.st_ino);
}
