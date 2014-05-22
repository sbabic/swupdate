/*
 * (C) Copyright 2013
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/fcntl.h>
#include <dirent.h>
#include "swupdate.h"
#include "util.h"
#include "fw_env.h"

/*
 * Replacement for fw_setenv() for calling inside
 * the library
 */
int fw_set_one_env (const char *name, const char *value)
{

	if (fw_env_open ()) {
		fprintf (stderr, "Error: environment not initialized\n");
		return -1;
	}
	fw_env_write ((char *)name, (char *)value);
	return fw_env_close ();
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
#ifdef HW_COMPATIBILITY_FILE
#define HW_FILE HW_COMPATIBILITY_FILE
#else
#define HW_FILE "/etc/hwrevision"
#endif
	/*
	 * Not all boards have pins for revision number
	 * check if there is a file containing theHW revision number
	 */

	hw->major = 0;
	hw->minor = 0;

	fp = fopen(HW_FILE, "r");
	if (!fp)
		return -1;

	if (fscanf(fp, "%d.%d", &hw->major, &hw->minor) == EOF)
		return -1;
	fclose(fp);

	return 0;
}

/*
 * The HW revision of the board *MUST* be inserted
 * in the sw-description file
 */
int check_hw_compatibility(struct swupdate_cfg *cfg)
{
	struct hw_type *hw, hwrev;
	int ret;

	ret = get_hw_revision(&hwrev);
	if (ret < 0)
		return -1;

	LIST_FOREACH(hw, &cfg->hardware, next) {
		if ((hw->major == hwrev.major) && (hw->minor == hwrev.minor))
			return 0;
	}

	return -1;
}
