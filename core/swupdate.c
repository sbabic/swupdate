/*
 * (C) Copyright 2012-2013
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
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <libgen.h>
#include <fnmatch.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>

#ifdef CONFIG_DOWNLOAD
#include <curl/curl.h>
#endif

#include "cpiohdr.h"
#include "util.h"
#include "swupdate.h"
#include "fw_env.h"
#include "parsers.h"
#include "network_interface.h"
#include "handler.h"
#include "installer.h"
#include "flash.h"
#include "lua_util.h"

#define MODULE_NAME	"swupdate"

/* Tree derived from the configuration file */
static struct swupdate_cfg swcfg;

/* Global MTD configuration */
static struct flash_description flashdesc;

struct flash_description *get_flash_info(void) {
	return &flashdesc;
}

static struct option long_options[] = {
	{"verbose", no_argument, NULL, 'v'},
	{"image", required_argument, NULL, 'i'},
	{"blacklist", required_argument, NULL, 'b'},
	{"help", no_argument, NULL, 'h'},
#ifdef CONFIG_DOWNLOAD
	{"download", required_argument, NULL, 'd'},
#endif
#ifdef CONFIG_WEBSERVER
	{"webserver", required_argument, NULL, 'w'},
#endif
	{NULL, 0, NULL, 0}
};

char main_options[20];

int verbose = 0;

static void usage(char *programname)
{
	printf("%s (compiled %s)\n", programname, __DATE__);
	printf(("Usage %s [OPTION]\n"
		" -v, --verbose         : be verbose\n"
		" -i, --image <filename> : Software to be installed\n"
		" -b, --blacklist <list of mtd> : MTDs that must not be scanned for UBI\n"
#ifdef CONFIG_DOWNLOAD
		" -d, --download <url> : URL of image to be downloaded. Image will be\n"
		"                        downloaded completely to --image filename, then\n"
		"                        installation will proceed as usual.\n"
#endif
#ifdef CONFIG_WEBSERVER
		" -w, --webserver [OPTIONS] : Parameters to be passed to webserver\n"
#endif
		" -h, --help            : print this help and exit\n"),
	       programname);
}

static int check_provided(struct imglist *list)
{
	int ret = 0;
	struct img_type *p;

	for (p = list->lh_first; p != NULL;
		p = p->next.le_next) {
		if (!p->provided) {
			ERROR("Requested file not found in image: %s", \
				p->fname);
			ret = -1;
		}
	}

	return ret;
}

static int searching_for_image(char *name)
{
	char *dir, *dirc, *basec;
	char *fpattern;
	DIR *path;
	struct dirent *dp;
	int i;
	int fd = -1;
	int found;
	char fname[MAX_IMAGE_FNAME];
	char *buf;
	char hex[4];

	dirc = strdup(name);
	basec = strdup(name);
	dir = dirname(dirc);
	fpattern = basename(basec);
	path = opendir(dir);

	TRACE("Searching image: check %s into %s\n",
			basec, dirc);
	if (!path) {
		free(dirc);
		free(basec);
		return -EBADF;
	}

	dp = readdir(path);
	do {
		if (!dp)
			break;
		if (!strcmp(dp->d_name, ".") ||
				!strcmp(dp->d_name, "..") ||
				!strlen(dp->d_name))
			continue;
		found = !fnmatch(fpattern, dp->d_name, FNM_CASEFOLD);

		if (found) {
			TRACE("File found: %s :\n", dp->d_name);
			/* Buffer for hexa output */
			buf = (char *)malloc(3 * strlen(dp->d_name) + 1);
			if (buf) {
				for (i = 0; i < strlen(dp->d_name); i++) {
					snprintf(hex, sizeof(hex), "%x ", dp->d_name[i]);
					memcpy(&buf[3 * i], hex, 3);
				}
				buf[3 * strlen(dp->d_name)] = '\0';
				TRACE("File name (hex): %s\n", buf);
			}
			/* Take the first one as image */
			if (fd < 0) {
				snprintf(fname, sizeof(fname), "%s/%s", dirc, dp->d_name);
				fd = open(fname, O_RDONLY);
				if (fd > 0)
					TRACE("\t\t**Used for upgrade\n");
			}
			free(buf);
		}

	} while ((dp = readdir(path)) !=NULL);

	free(dirc);
	free(basec);

	return fd;
}


static int install_from_file(char *fname)
{
	int fdsw;
	off_t pos;
	int ret;


	if (!strlen(fname)) {
		ERROR("Image not found...please reboot\n");
		exit(1);
	}

	fdsw = open(fname, O_RDONLY);
	if (fdsw < 0) {
		fdsw = searching_for_image(fname);
		if (fdsw < 0) {
			ERROR("Image Software cannot be read...exiting !\n");
			exit(1);
		}
	}

	pos = extract_sw_description(fdsw);
	ret = parse(&swcfg, TMPDIR SW_DESCRIPTION_FILENAME);
	if (ret) {
		exit(1);
	}


	if (check_hw_compatibility(&swcfg)) {
		ERROR("SW not compatible with hardware\n");
		exit(1);
	}

	if (cpio_scan(fdsw, &swcfg, pos) < 0) {
		close(fdsw);
		exit(1);
	}

	/*
	 * Check if all files described in sw-description
	 * are in the image
	 */
	ret = check_provided(&swcfg.images);
	ret |= check_provided(&swcfg.files);
	ret |= check_provided(&swcfg.scripts);
	if (ret)
		exit(1);

	/* copy images */
	ret = install_images(&swcfg, fdsw, 1);

	close(fdsw);

	if (ret) {
		fprintf(stdout, "Software updated failed\n");
		exit(1);
	}

	fprintf(stdout, "Software updated successfully\n");
	fprintf(stdout, "Please reboot the device to start the new software\n");

	return 0;
}

#ifdef CONFIG_DOWNLOAD
static int download_from_url(char *image_url, char *fname)
{
	CURL *curl_handle;
	CURLcode res;
	FILE *image;

	if (!strlen(image_url)) {
		ERROR("Image URL not provided... aborting download and update\n");
		exit(1);
	}

	if (!strlen(fname)) {
		ERROR("Image name not provided... aborting download and update\n");
		exit(1);
	}

	image = fopen(fname, "w");
	if (image == NULL) {
		ERROR("Image file cannot be written...exiting!\n");
		exit(1);
	}

	puts("Image download started");
	notify(DOWNLOAD, 0, 0);

	curl_global_init(CURL_GLOBAL_ALL);
	curl_handle = curl_easy_init();

	/* Write all data to the image file */
	curl_easy_setopt(curl_handle, CURLOPT_URL, image_url);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, image);
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "swupdate");

	/* TODO: Convert this to a streaming download at some point such
	 * that the file doesn't need to be downloaded completely before
	 * unpacking it for updating. See stream_interface for example. */
	if ((res = curl_easy_perform(curl_handle)) != CURLE_OK) {
		ERROR("Failed to download image: %s, exiting!\n",
				curl_easy_strerror(res));
		exit(1);
	}

	fclose(image);

	curl_easy_cleanup(curl_handle);
	curl_global_cleanup();

	puts("Image download completed");

	return 0;
}
#endif

static void swupdate_init(struct swupdate_cfg *sw)
{
	/* Initialize internal tree to store configuration */
	memset(sw, 0, sizeof(*sw));
	LIST_INIT(&sw->images);
	LIST_INIT(&sw->files);
	LIST_INIT(&sw->partitions);
	LIST_INIT(&sw->hardware);
	LIST_INIT(&sw->scripts);
	LIST_INIT(&sw->uboot);


	/* Create directories for scripts */
	mkdir(SCRIPTS_DIR, 0777);
	mkdir(DATASRC_DIR, 0777);
	mkdir(DATADST_DIR, 0777);

	mtd_init();
	ubi_init();
}

int main(int argc, char **argv)
{
	int c;
	char fname[MAX_IMAGE_FNAME];
	int opt_i = 0;
	struct hw_type hwrev;
#ifdef CONFIG_DOWNLOAD
	char image_url[MAX_URL];
	int opt_d = 0;
#endif
#ifdef CONFIG_WEBSERVER
	char weboptions[1024];
	char **av = NULL;
	int ac = 0;
	int opt_w = 0;
#endif

	memset(&flashdesc, 0, sizeof(flashdesc));
	memset(main_options, 0, sizeof(main_options));
	strcpy(main_options, "vhi:b:");
#ifdef CONFIG_DOWNLOAD
	strcat(main_options, "d:");
#endif
#ifdef CONFIG_WEBSERVER
	strcat(main_options, "w:");
#endif
	memset(fname, 0, sizeof(fname));

	printf("%s\n", BANNER);
	printf("Licensed under GPLv2. See source distribution for detailed "
		"copyright notices.\n\n");

	/* Process options with getopt */
	while ((c = getopt_long(argc, argv, main_options,
				long_options, NULL)) != EOF) {
		switch (c) {
		case 'v':
			verbose++;
			break;
		case 'b':
			mtd_set_ubiblacklist(optarg);
		case 'i':
			strncpy(fname, optarg, sizeof(fname));
			opt_i = 1;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
			break;
#ifdef CONFIG_DOWNLOAD
		case 'd':
			strncpy(image_url, optarg, sizeof(image_url));
			opt_d = 1;
			break;
#endif
#ifdef CONFIG_WEBSERVER
		case 'w':
			snprintf(weboptions, sizeof(weboptions), "%s %s", argv[0], optarg);
			av = splitargs(weboptions, &ac);
			opt_w = 1;
			break;
#endif
		default:
			usage(argv[0]);
			exit(1);
			break;
		}
	}

	swupdate_init(&swcfg);
	lua_handlers_init();
	if(!get_hw_revision(&hwrev))
		printf("Running on %s Revision %s\n", hwrev.boardname, hwrev.revision);

	print_registered_handlers();
	notify_init();

	if (opt_i) {
#ifdef CONFIG_DOWNLOAD
		if (opt_d) {
			download_from_url(image_url, fname);
		}
#endif

		install_from_file(fname);
		cleanup_files(&swcfg);

		notify(SUCCESS, 0, 0);
	}
#ifdef CONFIG_WEBSERVER
	if (opt_w)
		network_initializer(ac, av, &swcfg);
#endif
}	
