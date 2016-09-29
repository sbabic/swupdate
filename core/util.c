/*
 * (C) Copyright 2013
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include "swupdate.h"
#include "util.h"
#include "fw_env.h"
#include "generated/autoconf.h"

struct decryption_key {
	unsigned char key[32];
	unsigned char ivt[16];
};

static struct decryption_key *aes_key = NULL;

char *sdup(const char *str) {
	char *p;
	if ((p = (char *) malloc(strlen(str) + 1)) != NULL) {
		strcpy(p, str);
	}
	return p;
}

/*
 * Replacement for fw_setenv() for calling inside
 * the library
 */
#ifdef CONFIG_UBOOT
int fw_set_one_env (const char *name, const char *value)
{

	if (fw_env_open (NULL)) {
		fprintf (stderr, "Error: environment not initialized\n");
		return -1;
	}
	fw_env_write ((char *)name, (char *)value);
	return fw_env_close (NULL);
}
#else
int fw_set_one_env (const char __attribute__ ((__unused__)) *name,
			const char __attribute__ ((__unused__)) *value)
{
	return 0;
}
#endif

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
		&& (argv = malloc((argn + 1) * sizeof (char *)))) {
			*argv++ = args;
			argn = countargc(args, argv);
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
		ERROR("I cannot open %s %d\n", filename, errno);

	return fdout;
}

int isDirectoryEmpty(const char *dirname)
{
	int n = 0;
	struct dirent *d;
	DIR *dir = opendir(dirname);

	if (dir == NULL)
		return 1;

	while ((d = readdir(dir)) != NULL) {
		if(++n > 2)
			break;
  	}
	closedir(dir);

	if (n <= 2)
		return 1;

	return 0;
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
		TRACE("Cannot find Board Revision\n");
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
 * Read versions of components from a file, if provided
 * This is used to check for version mismatch and avoid
 * to reinstall a component that is already installed
 */
#ifdef CONFIG_SW_VERSIONS_FILE
#define SW_VERSIONS_FILE CONFIG_SW_VERSIONS_FILE
#else
#define SW_VERSIONS_FILE "/etc/sw-versions"
#endif
void get_sw_versions(struct swupdate_cfg *sw)
{
	FILE *fp;
	int ret;
	char *name, *version;
	struct sw_version *swcomp;

	/*
	 * scan all entries inside SW_VERSIONS_FILE
	 * and generate a list
	 */

	fp = fopen(SW_VERSIONS_FILE, "r");
	if (!fp)
		return;

	while (1) {
		ret = fscanf(fp, "%ms %ms", &name, &version);
		/* pair component / version found */
		if (ret == 2) {
			swcomp = (struct sw_version *)calloc(1, sizeof(struct sw_version));
			if (!swcomp) {
				ERROR("Allocation error");
				return;
			}
			strncpy(swcomp->name, name, sizeof(swcomp->name));
			strncpy(swcomp->version, version, sizeof(swcomp->version));
			LIST_INSERT_HEAD(&sw->installed_sw_list, swcomp, next);
			TRACE("Installed %s: Version %s",
					swcomp->name,
					swcomp->version);
			free(name);
			free(version);
		} else {
			if (ret == EOF)
				break;
			if (errno) {
				ERROR("Malformed sw-versions file, skipped !");
				break;
			}

			/*
			 * Malformed file, skip the line
			 * and check next
			 */
			if (ret == 1)
				free(name);
		}
	}
	fclose(fp);
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
			ERROR("Malformed number %.*s\n", (int)digs, where);
			break;
		}

		d = p - codetab;
		if ((d >> logbase) > 1)
		{
			ERROR("Malformed number %.*s\n", (int)digs, where);
			break;
		}
		value += d;
		if (++buf == end || *buf == 0)
			break;
		overflow |= value ^ (value << logbase >> logbase);
		value <<= logbase;
	}
	if (overflow)
		ERROR("Archive value %.*s is out of range\n",
			(int)digs, where);
	return value;
}

/*
 * Convert a hash as hexa string into a sequence of bytes
 * hash must be a an array of 32 bytes as specified by SHA256
 */
static int ascii_to_bin(unsigned char *hash, const char *s, int len)
{
	int i;
	unsigned int val;

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
	char *b1, *b2;
	int ret;

	fp = fopen(fname, "r");
	if (!fp)
		return -EBADF;

	ret = fscanf(fp, "%ms %ms", &b1, &b2);
	fclose(fp);

	if (aes_key)
		free(aes_key);

	aes_key = (struct decryption_key *)calloc(1, sizeof(*aes_key));
	if (!aes_key)
		return -ENOMEM;

	if (ret != 2) {
		fprintf(stderr, "File with decryption key is in the format <key> <ivt>\n");
		return -EINVAL;
	}

	/*
	 * Key is for aes_256, it must be 256 bit
	 * and IVT is 128 bit
	 */
	ret = ascii_to_bin(aes_key->key, b1, 64) | ascii_to_bin(aes_key->ivt, b2, 32); 

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
