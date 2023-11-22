/*
 * (C) Copyright 2013-2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <sys/types.h>
#include <stdbool.h>
#include "bsdqueue.h"
#include "globals.h"
#include "mongoose_interface.h"
#include "swupdate_dict.h"
#include "swupdate_image.h"
#include "hw-compatibility.h"

/*
 * this is used to indicate if a file
 * in the .swu image is required for the
 * device, or can be skipped
 */
typedef enum {
	COPY_FILE,
	SKIP_FILE,
	INSTALL_FROM_STREAM
} swupdate_file_t;

struct extproc {
	char name[SWUPDATE_GENERAL_STRING_SIZE];
	char exec[SWUPDATE_GENERAL_STRING_SIZE];
	char options[SWUPDATE_GENERAL_STRING_SIZE];
	LIST_ENTRY(extproc) next;
};

LIST_HEAD(proclist, extproc);

struct swupdate_parms {
	bool dry_run;
	char software_set[SWUPDATE_GENERAL_STRING_SIZE];
	char running_mode[SWUPDATE_GENERAL_STRING_SIZE];
};

struct swupdate_cfg {
	char name[SWUPDATE_GENERAL_STRING_SIZE];
	char description[SWUPDATE_UPDATE_DESCRIPTION_STRING_SIZE];
	char version[SWUPDATE_GENERAL_STRING_SIZE];
	bool bootloader_transaction_marker;
	bool bootloader_state_marker;
	char output[SWUPDATE_GENERAL_STRING_SIZE];
	char publickeyfname[SWUPDATE_GENERAL_STRING_SIZE];
	char aeskeyfname[SWUPDATE_GENERAL_STRING_SIZE];
	char postupdatecmd[SWUPDATE_GENERAL_STRING_SIZE];
	char preupdatecmd[SWUPDATE_GENERAL_STRING_SIZE];
	char minimum_version[SWUPDATE_GENERAL_STRING_SIZE];
	char maximum_version[SWUPDATE_GENERAL_STRING_SIZE];
	char current_version[SWUPDATE_GENERAL_STRING_SIZE];
	char mtdblacklist[SWUPDATE_GENERAL_STRING_SIZE];
	char forced_signer_name[SWUPDATE_GENERAL_STRING_SIZE];
	char namespace_for_vars[SWUPDATE_GENERAL_STRING_SIZE];
	void *lua_state;
	bool syslog_enabled;
	bool no_downgrading;
	bool no_reinstalling;
	bool no_transaction_marker;
	bool no_state_marker;
	bool reboot_required;
	bool check_max_version;
	int verbose;
	int loglevel;
	int cert_purpose;
	struct hw_type hw;
	struct hwlist hardware;
	struct swver installed_sw_list;
	struct imglist images;
	struct imglist scripts;
	struct imglist bootscripts;
	struct dict bootloader;
	struct dict vars;
	struct dict accepted_set;
	struct proclist extprocs;
	void *dgst;	/* Structure for signed images */
	struct swupdate_parms parms;
	const char *embscript;
	char gpg_home_directory[SWUPDATE_GENERAL_STRING_SIZE];
	char gpgme_protocol[SWUPDATE_GENERAL_STRING_SIZE];
};

#define SEARCH_FILE(img, list, found, offs) do { \
	if (!found) { \
		for (img = list.lh_first; img != NULL; \
			img = img->next.le_next) { \
			if (strcmp(img->fname, fdh.filename) == 0) { \
				found = 1; \
				img->offset = offs; \
				img->provided = 1; \
				img->size = fdh.size; \
			} \
		} \
		if (!found) \
			img = NULL; \
	} \
} while(0)

int cpio_scan(int fd, struct swupdate_cfg *cfg, off_t start);
struct swupdate_cfg *get_swupdate_cfg(void);
void free_image(struct img_type *img);
