/*
 * (C) Copyright 2012-2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <ftw.h>

#include "bsdqueue.h"
#include "cpiohdr.h"
#include "util.h"
#include "swupdate.h"
#include "parsers.h"
#include "network_interface.h"
#include "handler.h"
#include "installer.h"
#ifdef CONFIG_MTD
#include "flash.h"
#endif
#include "lua_util.h"
#include "mongoose_interface.h"
#include "download_interface.h"
#include "network_ipc.h"
#include "sslapi.h"
#include "suricatta/suricatta.h"
#include "progress.h"
#include "parselib.h"
#include "swupdate_settings.h"
#include "pctl.h"
#include "state.h"

#ifdef CONFIG_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#define MODULE_NAME	"swupdate"

static pthread_t network_daemon;

/* Tree derived from the configuration file */
static struct swupdate_cfg swcfg;

#ifdef CONFIG_MTD
/* Global MTD configuration */
static struct flash_description flashdesc;

struct flash_description *get_flash_info(void) {
	return &flashdesc;
}
#endif

static struct option long_options[] = {
	{"verbose", no_argument, NULL, 'v'},
	{"version", no_argument, NULL, '0'},
	{"image", required_argument, NULL, 'i'},
	{"file", required_argument, NULL, 'f'},
	{"loglevel", required_argument, NULL, 'l'},
	{"syslog", no_argument, NULL, 'L' },
	{"select", required_argument, NULL, 'e'},
	{"accepted-select", required_argument, NULL, 'q'},
	{"output", required_argument, NULL, 'o'},
	{"dry-run", no_argument, NULL, 'n'},
	{"no-downgrading", required_argument, NULL, 'N'},
	{"no-reinstalling", required_argument, NULL, 'R'},
	{"no-transaction-marker", no_argument, NULL, 'M'},
#ifdef CONFIG_SIGNED_IMAGES
	{"key", required_argument, NULL, 'k'},
	{"ca-path", required_argument, NULL, 'k'},
	{"cert-purpose", required_argument, NULL, '1'},
	{"forced-signer-name", required_argument, NULL, '2'},
#endif
#ifdef CONFIG_ENCRYPTED_IMAGES
	{"key-aes", required_argument, NULL, 'K'},
#endif
#ifdef CONFIG_UBIATTACH
	{"blacklist", required_argument, NULL, 'b'},
#endif
	{"help", no_argument, NULL, 'h'},
#ifdef CONFIG_HW_COMPATIBILITY
	{"hwrevision", required_argument, NULL, 'H'},
#endif
#ifdef CONFIG_DOWNLOAD
	{"download", required_argument, NULL, 'd'},
#endif
#ifdef CONFIG_SURICATTA
	{"suricatta", required_argument, NULL, 'u'},
#endif
#ifdef CONFIG_WEBSERVER
	{"webserver", required_argument, NULL, 'w'},
#endif
	{"check", no_argument, NULL, 'c'},
	{"postupdate", required_argument, NULL, 'p'},
	{"preupdate", required_argument, NULL, 'P'},
	{NULL, 0, NULL, 0}
};

int loglevel = ERRORLEVEL;

static void usage(char *programname)
{
	fprintf(stdout, "%s (compiled %s)\n", programname, __DATE__);
	fprintf(stdout, "Usage %s [OPTION]\n",
			programname);
	fprintf(stdout,
		" -f, --file <filename>          : configuration file to use\n"
#ifdef CONFIG_UBIATTACH
		" -b, --blacklist <list of mtd>  : MTDs that must not be scanned for UBI\n"
#endif
		" -p, --postupdate               : execute post-update command\n"
		" -P, --preupdate                : execute pre-update command\n"
		" -e, --select <software>,<mode> : Select software images set and source\n"
		"                                  Ex.: stable,main\n"
		" --accepted-select\n"
		"            <software>,<mode>   : List for software images set and source\n"
		"                                  that are accepted via IPC\n"
		"                                  Ex.: stable,main\n"
		"                                  it can be set multiple times\n"
		" -i, --image <filename>         : Software to be installed\n"
		" -l, --loglevel <level>         : logging level\n"
		" -L, --syslog                   : enable syslog logger\n"
#ifdef CONFIG_SIGNED_IMAGES
		" -k, --key <public key file>    : file with public key to verify images\n"
		"     --cert-purpose <purpose>   : set expected certificate purpose\n"
		"                                  [emailProtection|codeSigning] (default: emailProtection)\n"
		"     --forced-signer-name <cn>  : set expected common name of signer certificate\n"
		"     --ca-path                  : path to the Certificate Authority (PEM)\n"
#endif
#ifdef CONFIG_ENCRYPTED_IMAGES
		" -K, --key-aes <key file>       : the file contains the symmetric key to be used\n"
		"                                  to decrypt images\n"
#endif
		" -n, --dry-run                  : run SWUpdate without installing the software\n"
		" -N, --no-downgrading <version> : not install a release older as <version>\n"
		" -R, --no-reinstalling <version>: not install a release same as <version>\n"
		" -M, --no-transaction-marker    : disable setting bootloader transaction marker\n"
		" -o, --output <filename>        : saves the incoming stream\n"
		" -v, --verbose                  : be verbose, set maximum loglevel\n"
		"     --version                  : print SWUpdate version and exit\n"
#ifdef CONFIG_HW_COMPATIBILITY
		" -H, --hwrevision <board>:<rev> : Set hardware revision\n"
#endif
		" -c, --check                    : check image and exit, use with -i <filename>\n"
		" -h, --help                     : print this help and exit\n"
		);
#ifdef CONFIG_DOWNLOAD
	fprintf(stdout,
		" -d, --download [OPTIONS]       : Parameters to be passed to the downloader\n");
	download_print_help();
#endif
#ifdef CONFIG_SURICATTA
	fprintf(stdout,
		" -u, --suricatta [OPTIONS]      : Parameters to be passed to suricatta\n");
	suricatta_print_help();
#endif
#ifdef CONFIG_WEBSERVER
	fprintf(stdout,
		" -w, --webserver [OPTIONS]      : Parameters to be passed to webserver\n");
	mongoose_print_help();
#endif
}

static int check_provided(struct imglist *list)
{
	int ret = 0;
	struct img_type *p;

	for (p = list->lh_first; p != NULL;
		p = p->next.le_next) {
		if (!p->provided && !(get_handler_mask(p) & NO_DATA_HANDLER)) {
			ERROR("Requested file not found in image: %s", \
				p->fname);
			ret = -1;
		}
	}

	return ret;
}

struct swupdate_cfg *get_swupdate_cfg(void) {
	return &swcfg;
}

/*
 * Extract board and revision number from command line
 * The parameter is in the format <board>:<revision>
 */
static int opt_to_hwrev(char *param, struct hw_type *hw)
{
	char *s;

	/* skip if there is no string */
	if (!param)
		return 0;

	s = strchr(param, ':');

	if (!s) {
		ERROR("You pass Hardware Revision in wrong format: %s",
				param);
		return -EINVAL;
	}

	strlcpy(hw->revision, s + 1, sizeof(hw->revision));
	*s = '\0';
	strlcpy(hw->boardname, param, sizeof(hw->boardname));

	if (!strlen(hw->boardname) || !strlen(hw->revision))
		return -EINVAL;

	return 0;
}

static int searching_for_image(char *name)
{
	char *dir, *dirc, *basec;
	char *fpattern;
	DIR *path;
	struct dirent *dp;
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

	TRACE("Searching image: check %s into %s",
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
			TRACE("File found: %s :", dp->d_name);
			/* Buffer for hexa output */
			buf = (char *)malloc(3 * strlen(dp->d_name) + 1);
			if (buf) {
				for (size_t i = 0; i < strlen(dp->d_name); i++) {
					snprintf(hex, sizeof(hex), "%x ", dp->d_name[i]);
					memcpy(&buf[3 * i], hex, 3);
				}
				buf[3 * strlen(dp->d_name)] = '\0';
				TRACE("\nFile name (hex): %s", buf);
			}
			/* Take the first one as image */
			if (fd < 0) {
				if (snprintf(fname, sizeof(fname), "%s/%s",
					     dirc, dp->d_name) >= (int)sizeof(fname)) {
					ERROR("Path too long: %s/%s", dirc, dp->d_name);
				}
				fd = open(fname, O_RDONLY);
				if (fd > 0)
					TRACE("\t\t**Used for upgrade");
			}
			free(buf);
		}

	} while ((dp = readdir(path)) !=NULL);

	free(dirc);
	free(basec);
	closedir(path);

	return fd;
}

static int install_from_file(char *fname, int check)
{
	int fdsw;
	off_t pos;
	int ret;


	if (!strlen(fname)) {
		ERROR("Image not found...please reboot");
		exit(EXIT_FAILURE);
	}

	fdsw = open(fname, O_RDONLY);
	if (fdsw < 0) {
		fdsw = searching_for_image(fname);
		if (fdsw < 0) {
			ERROR("Image Software cannot be read...exiting !");
			exit(EXIT_FAILURE);
		}
	}

	pos = 0;
	ret = extract_sw_description(fdsw, SW_DESCRIPTION_FILENAME, &pos);
#ifdef CONFIG_SIGNED_IMAGES
	ret |= extract_sw_description(fdsw, SW_DESCRIPTION_FILENAME ".sig",
		&pos);
#endif
	/*
	 * Check if files could be extracted
	 */
	if (ret) {
		ERROR("Failed to extract meta information");
		exit(EXIT_FAILURE);
	}

	char* swdescfilename = alloca(strlen(get_tmpdir())+strlen(SW_DESCRIPTION_FILENAME)+1);
	sprintf(swdescfilename, "%s%s", get_tmpdir(), SW_DESCRIPTION_FILENAME);
	ret = parse(&swcfg, swdescfilename);
	if (ret) {
		ERROR("failed to parse " SW_DESCRIPTION_FILENAME "!");
		exit(EXIT_FAILURE);
	}

	if (check_hw_compatibility(&swcfg)) {
		ERROR("SW not compatible with hardware");
		exit(EXIT_FAILURE);
	}

	if (cpio_scan(fdsw, &swcfg, pos) < 0) {
		ERROR("failed to scan for pos '%lld'!", (long long)pos);
		close(fdsw);
		exit(EXIT_FAILURE);
	}

	/*
	 * Check if all files described in sw-description
	 * are in the image
	 */
	ret = check_provided(&swcfg.images);
	if (ret) {
		ERROR("failed to check images!");
		exit(EXIT_FAILURE);
	}
	ret = check_provided(&swcfg.scripts);
	if (ret) {
		ERROR("failed to check scripts!");
		exit(EXIT_FAILURE);
	}

	if (check) {
		fprintf(stdout, "successfully checked '%s'\n", fname);
		exit(EXIT_SUCCESS);
	}

	ret = preupdatecmd(&swcfg);
	if (ret) {
		ERROR("Failed pre-update command!");
		exit(EXIT_FAILURE);
	}

#ifdef CONFIG_MTD
		mtd_cleanup();
		scan_mtd_devices();
#endif
	/*
	 * Set "recovery_status" as begin of the transaction"
	 */
	if (swcfg.bootloader_transaction_marker) {
		save_state_string((char*)BOOTVAR_TRANSACTION, STATE_IN_PROGRESS);
	}

	ret = install_images(&swcfg, fdsw, 1);

	swupdate_progress_end(ret == 0 ? SUCCESS : FAILURE);

	close(fdsw);

	if (ret) {
		fprintf(stdout, "Software update failed\n");
		return EXIT_FAILURE;
	}

	if (swcfg.bootloader_transaction_marker) {
		unset_state((char*)BOOTVAR_TRANSACTION);
	}
	fprintf(stdout, "Software updated successfully\n");
	fprintf(stdout, "Please reboot the device to start the new software\n");

	return EXIT_SUCCESS;
}

static int parse_image_selector(const char *selector, struct swupdate_cfg *sw)
{
	char *pos;

	pos = strchr(selector, ',');
	if (pos == NULL)
		return -EINVAL;

	*pos = '\0';

	/*
	 * the runtime copy in swcfg can be overloaded by IPC,
	 * so maintain a copy to restore it after an update
	 */
	strlcpy(sw->globals.default_software_set, selector, sizeof(sw->globals.default_software_set));
	strlcpy(sw->software_set, selector, sizeof(sw->software_set));
	/* pos + 1 will either be NULL or valid text */
	strlcpy(sw->globals.default_running_mode, pos + 1, sizeof(sw->globals.default_running_mode));
	strlcpy(sw->running_mode, pos + 1, sizeof(sw->running_mode));

	if (strlen(sw->software_set) == 0 || strlen(sw->running_mode) == 0)
		return -EINVAL;

	return 0;
}

static void create_directory(const char* path) {
	char* dpath;
	if (asprintf(&dpath, "%s%s", get_tmpdir(), path) ==
		ENOMEM_ASPRINTF) {
		ERROR("OOM: Directory %s not created", path);
		return;
	}
	if (mkdir(dpath, 0777)) {
		WARN("Directory %s cannot be created due to : %s",
		     path, strerror(errno));
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

static int remove_directory(const char* path)
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

static void swupdate_cleanup(void)
{
#ifndef CONFIG_NOCLEANUP
	remove_directory(SCRIPTS_DIR_SUFFIX);
	remove_directory(DATADST_DIR_SUFFIX);
#endif
}

static void swupdate_init(struct swupdate_cfg *sw)
{
	/* Initialize internal tree to store configuration */
	memset(sw, 0, sizeof(*sw));
	LIST_INIT(&sw->images);
	LIST_INIT(&sw->hardware);
	LIST_INIT(&sw->scripts);
	LIST_INIT(&sw->bootscripts);
	LIST_INIT(&sw->bootloader);
	LIST_INIT(&sw->extprocs);
	sw->globals.cert_purpose = SSL_PURPOSE_DEFAULT;


	/* Create directories for scripts */
	create_directory(SCRIPTS_DIR_SUFFIX);
	create_directory(DATADST_DIR_SUFFIX);

	if (atexit(swupdate_cleanup) != 0) {
		TRACE("Cannot setup SWUpdate cleanup on exit");
	}

#ifdef CONFIG_MTD
	mtd_init();
	ubi_init();
#endif
}

static int parse_cert_purpose(const char *text)
{
	static const char CODE_SIGN[] = "codeSigning";
	static const char EMAIL_PROT[] = "emailProtection";

	if (strncmp(CODE_SIGN, text, sizeof(CODE_SIGN)) == 0)
		return SSL_PURPOSE_CODE_SIGN;

	if (strncmp(EMAIL_PROT, text, sizeof(EMAIL_PROT)) == 0)
		return SSL_PURPOSE_EMAIL_PROT;

	ERROR("unknown certificate purpose '%s'\n", text);
	exit(EXIT_FAILURE);
}

static int read_globals_settings(void *elem, void *data)
{
	char tmp[SWUPDATE_GENERAL_STRING_SIZE] = "";
	struct swupdate_cfg *sw = (struct swupdate_cfg *)data;

	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"public-key-file", sw->globals.publickeyfname);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"ca-path", sw->globals.publickeyfname);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"aes-key-file", sw->globals.aeskeyfname);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"mtd-blacklist", sw->globals.mtdblacklist);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"postupdatecmd", sw->globals.postupdatecmd);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"preupdatecmd", sw->globals.preupdatecmd);
	get_field(LIBCFG_PARSER, elem, "verbose", &sw->globals.verbose);
	get_field(LIBCFG_PARSER, elem, "loglevel", &sw->globals.loglevel);
	get_field(LIBCFG_PARSER, elem, "syslog", &sw->globals.syslog_enabled);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"no-downgrading", sw->globals.minimum_version);
	if (strlen(sw->globals.minimum_version))
		sw->globals.no_downgrading = 1;
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"no-reinstalling", sw->globals.current_version);
	if (strlen(sw->globals.current_version))
		sw->globals.no_reinstalling = 1;
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"cert-purpose", tmp);
	if (tmp[0] != '\0')
		sw->globals.cert_purpose = parse_cert_purpose(tmp);
	GET_FIELD_STRING(LIBCFG_PARSER, elem, "forced-signer-name",
				sw->globals.forced_signer_name);

	return 0;
}

const char *loglevnames[] = {
	[ERRORLEVEL] = "error",
	[WARNLEVEL] = "warning",
	[INFOLEVEL] = "info",
	[DEBUGLEVEL] = "debug",
	[TRACELEVEL] = "trace"
};

static int read_console_settings(void *elem, void __attribute__ ((__unused__)) *data)
{
	char tmp[SWUPDATE_GENERAL_STRING_SIZE] = "";
	int i;

	for (i = ERRORLEVEL; i <= LASTLOGLEVEL; i++) {
		memset(tmp, 0, sizeof(tmp));
		GET_FIELD_STRING(LIBCFG_PARSER, elem, loglevnames[i], tmp);
		if (tmp[0] != '\0')
			notifier_set_color(i, tmp);
	}
	return 0;
}

static int read_processes_settings(void *settings, void *data)
{
	struct swupdate_cfg *sw = (struct swupdate_cfg *)data;
	void *elem;
	int count, i;
	struct extproc *proc, *last = NULL;

	count = get_array_length(LIBCFG_PARSER, settings);

	for(i = 0; i < count; ++i) {
		elem = get_elem_from_idx(LIBCFG_PARSER, settings, i);

		if (!elem)
			continue;

		if(!(exist_field_string(LIBCFG_PARSER, elem, "name")))
			continue;
		if(!(exist_field_string(LIBCFG_PARSER, elem, "exec")))
			continue;

		proc = (struct extproc *)calloc(1, sizeof(struct extproc));

		GET_FIELD_STRING(LIBCFG_PARSER, elem, "name", proc->name);
		GET_FIELD_STRING(LIBCFG_PARSER, elem, "exec", proc->exec);

		if (!last)
			LIST_INSERT_HEAD(&sw->extprocs, proc, next);
		else
			LIST_INSERT_AFTER(last, proc, next);

		last = proc;

		TRACE("External process \"%s\": \"%s %s\" will be started",
		       proc->name, proc->exec, proc->options);
	}

	return 0;
}

static void sigterm_handler(int __attribute__ ((__unused__)) signum)
{
	pthread_cancel(network_daemon);
}

int main(int argc, char **argv)
{
	int c;
	char fname[MAX_IMAGE_FNAME];
	char *cfgfname = NULL;
	const char *software_select = NULL;
	int opt_i = 0;
	int opt_e = 0;
	int opt_c = 0;
	char image_url[MAX_URL];
	char main_options[256];
	unsigned int public_key_mandatory = 0;
	struct sigaction sa;
	int result = EXIT_SUCCESS;
#ifdef CONFIG_SURICATTA
	int opt_u = 0;
	char *suricattaoptions;
	char **argvalues = NULL;
	int argcount = 0;
#endif
#ifdef CONFIG_WEBSERVER
	int opt_w = 0;
	char *weboptions;
	char **av = NULL;
	int ac = 0;
#endif
#ifdef CONFIG_DOWNLOAD
	int opt_d = 0;
	char *dwloptions;
#endif
	char **dwlav = NULL;
	int dwlac = 0;

#ifdef CONFIG_MTD
	memset(&flashdesc, 0, sizeof(flashdesc));
#endif
	memset(main_options, 0, sizeof(main_options));
	memset(image_url, 0, sizeof(image_url));
	strcpy(main_options, "vhni:e:q:l:Lcf:p:P:o:N:R:M");
#ifdef CONFIG_MTD
	strcat(main_options, "b:");
#endif
#ifdef CONFIG_DOWNLOAD
	strcat(main_options, "d:");
#endif
#ifdef CONFIG_SURICATTA
	strcat(main_options, "u:");
#endif
#ifdef CONFIG_WEBSERVER
	strcat(main_options, "w:");
#endif
#ifdef CONFIG_HW_COMPATIBILITY
	strcat(main_options, "H:");
#endif
#ifdef CONFIG_SIGNED_IMAGES
	strcat(main_options, "k:");
	public_key_mandatory = 1;
#endif
#ifdef CONFIG_ENCRYPTED_IMAGES
	strcat(main_options, "K:");
#endif

	memset(fname, 0, sizeof(fname));

	/* Initialize internal database */
	swupdate_init(&swcfg);

	/*
	 * Initialize notifier to enable at least output
	 * on the console
	 */
	notify_init();

	/*
	 * Check if there is a configuration file and parse it
	 * Parse once the command line just to find if a
	 * configuration file is passed
	 */
	while ((c = getopt_long(argc, argv, main_options,
				long_options, NULL)) != EOF) {
		switch (c) {
		case 'f':
			cfgfname = sdup(optarg);
			break;
		case '0':
			printf("%s", BANNER);
			exit(EXIT_SUCCESS);
		}
	}

	/* Check for (and use) default configuration if present and none
	 * was supplied on the command line (-f)
	 */
	if (cfgfname == NULL) {
		struct stat stbuf;
		if (stat(CONFIG_DEFAULT_CONFIG_FILE, &stbuf) == 0) {
			cfgfname = sdup(CONFIG_DEFAULT_CONFIG_FILE);
		}
	}

	/* Load configuration file */
	if (cfgfname != NULL) {
		/*
		 * 'globals' section is mandatory if configuration file is specified.
		 */
		int ret = read_module_settings(cfgfname, "globals",
					       read_globals_settings, &swcfg);
		if (ret != 0) {
			/*
			 * Exit on -ENODATA or -EINVAL errors.
			 */
			fprintf(stderr,
			    "Error parsing configuration file: %s, exiting.\n",
			    ret == -ENODATA ? "'globals' section missing"
					    : "cannot read");
			exit(EXIT_FAILURE);
		}

		loglevel = swcfg.globals.verbose ? TRACELEVEL : swcfg.globals.loglevel;

		/*
		 * The following sections are optional, hence -ENODATA error code is
		 * ignored if the section is not found. -EINVAL will not happen here.
		 */
		(void)read_module_settings(cfgfname, "logcolors",
					   read_console_settings, &swcfg);

		(void)read_module_settings(cfgfname, "processes",
					   read_processes_settings, &swcfg);
	}

	/*
	 * Command line should be parsed a second time
	 * This let line parameters overload
	 * setup in the configuration file
	 * According to getopt(), this can be done by setting to 0
	 * the external global optind variable
	 */
	optind = 0;

	/* Process options with getopt */
	while ((c = getopt_long(argc, argv, main_options,
				long_options, NULL)) != EOF) {
		if (optarg && *optarg == '-' && (c != 'd' && c != 'u' && c != 'w')) {
			/* An option's value starting with '-' is not allowed except
			 * for downloader, webserver, and suricatta doing their own
			 * argv parsing.
			 */
			c = '?';
		}
		switch (c) {
		case 'v':
			loglevel = TRACELEVEL;
			break;
#ifdef CONFIG_UBIATTACH
		case 'b':
			mtd_set_ubiblacklist(optarg);
			break;
#endif
		case 'i':
			strlcpy(fname, optarg, sizeof(fname));
			opt_i = 1;
			break;
		case 'o':
			strlcpy(swcfg.output, optarg, sizeof(swcfg.output));
			break;
		case 'l':
			loglevel = strtoul(optarg, NULL, 10);
			break;
		case 'n':
			swcfg.globals.dry_run = 1;
			break;
		case 'L':
			swcfg.globals.syslog_enabled = 1;
			break;
		case 'k':
			strlcpy(swcfg.globals.publickeyfname,
				optarg,
			       	sizeof(swcfg.globals.publickeyfname));
			break;
		case '1':
			swcfg.globals.cert_purpose = parse_cert_purpose(optarg);
			break;
		case '2':
			strlcpy(swcfg.globals.forced_signer_name, optarg,
				sizeof(swcfg.globals.forced_signer_name));
			break;
#ifdef CONFIG_ENCRYPTED_IMAGES
		case 'K':
			strlcpy(swcfg.globals.aeskeyfname,
				optarg,
			       	sizeof(swcfg.globals.aeskeyfname));
			break;
#endif
		case 'N':
			swcfg.globals.no_downgrading = 1;
			strlcpy(swcfg.globals.minimum_version, optarg,
				sizeof(swcfg.globals.minimum_version));
			break;
		case 'R':
			swcfg.globals.no_reinstalling = 1;
			strlcpy(swcfg.globals.current_version, optarg,
				sizeof(swcfg.globals.current_version));
			break;
		case 'M':
			swcfg.globals.no_transaction_marker = 1;
			TRACE("transaction_marker globally disabled");
			break;
		case 'e':
			software_select = optarg;
			opt_e = 1;
			break;
		/* Configuration file already parsed, ignores it */
		case 'f':
			break;
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;
#ifdef CONFIG_DOWNLOAD
		case 'd':
			if (asprintf(&dwloptions,"%s %s", argv[0], optarg) ==
			ENOMEM_ASPRINTF) {
				ERROR("Cannot allocate memory for downloader options.");
				exit(EXIT_FAILURE);
			}
			dwlav = splitargs(dwloptions, &dwlac);
			opt_d = 1;
			free(dwloptions);
			break;
#endif
		case 'H':
			if (opt_to_hwrev(optarg, &swcfg.hw) < 0)
				exit(EXIT_FAILURE);
			break;
		case 'q':
			dict_insert_value(&swcfg.accepted_set, "accepted", optarg);
			break;
#ifdef CONFIG_SURICATTA
		case 'u':
			if (asprintf(&suricattaoptions,"%s %s", argv[0], optarg) ==
			ENOMEM_ASPRINTF) {
				ERROR("Cannot allocate memory for suricatta options.");
				exit(EXIT_FAILURE);
			}
			argvalues = splitargs(suricattaoptions, &argcount);
			opt_u = 1;
			free(suricattaoptions);
			break;
#endif
#ifdef CONFIG_WEBSERVER
		case 'w':
			if (asprintf(&weboptions,"%s %s", argv[0], optarg) ==
			ENOMEM_ASPRINTF) {
				ERROR("Cannot allocate memory for webserver options.");
				exit(EXIT_FAILURE);
			}
			av = splitargs(weboptions, &ac);
			opt_w = 1;
			free(weboptions);
			break;
#endif
		case 'c':
			opt_c = 1;
			break;
		case 'p':
			strlcpy(swcfg.globals.postupdatecmd, optarg,
				sizeof(swcfg.globals.postupdatecmd));
			break;
		case 'P':
			strlcpy(swcfg.globals.preupdatecmd, optarg,
				sizeof(swcfg.globals.preupdatecmd));
			break;
		default:
			fprintf(stdout, "Try %s -h for usage\n", argv[0]);
			exit(EXIT_FAILURE);
			break;
		}
	}

	if (optind < argc) {
		/* SWUpdate has no non-option arguments, fail on them */
		fprintf(stderr,
			 "Error: Non-option or unrecognized argument(s) given, see --help.\n");
		exit(EXIT_FAILURE);
	}

	/*
	 * Parameters are parsed: now performs plausibility
	 * tests before starting processes and threads
	 */
	if (public_key_mandatory && !strlen(swcfg.globals.publickeyfname)) {
		fprintf(stderr,
			 "Error: SWUpdate is built for signed images, provide a public key file.\n");
		exit(EXIT_FAILURE);
	}

	if (opt_c && !opt_i) {
		fprintf(stderr,
			"Error: Checking local images requires -i <file>.\n");
		exit(EXIT_FAILURE);
	}

	if (opt_i && strlen(swcfg.output)) {
		fprintf(stderr,
			"Error: Use cp for -i <image> -o <outfile>.\n");
		exit(EXIT_FAILURE);
	}

#ifdef CONFIG_SURICATTA
	if (opt_u && (opt_c || opt_i)) {
		fprintf(stderr, "Error: Invalid mode combination with suricatta.\n");
		exit(EXIT_FAILURE);
	}
#endif

	swupdate_crypto_init();

	if (strlen(swcfg.globals.publickeyfname)) {
		if (swupdate_dgst_init(&swcfg, swcfg.globals.publickeyfname)) {
			fprintf(stderr,
				 "Error: Crypto cannot be initialized.\n");
			exit(EXIT_FAILURE);
		}
	}

	printf("%s\n", BANNER);
	printf("Licensed under GPLv2. See source distribution for detailed "
		"copyright notices.\n\n");

	/*
	 * Install a child handler to check if a subprocess
	 * dies
	 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigchld_handler;
	sigaction(SIGCHLD, &sa, NULL);
#ifdef CONFIG_UBIATTACH
	if (strlen(swcfg.globals.mtdblacklist))
		mtd_set_ubiblacklist(swcfg.globals.mtdblacklist);
#endif

	/*
	 * If an AES key is passed, load it to allow
	 * to decrypt images
	 */
	if (strlen(swcfg.globals.aeskeyfname)) {
		if (load_decryption_key(swcfg.globals.aeskeyfname)) {
			fprintf(stderr,
				"Error: Key file does not contain a valid AES key.\n");
			exit(EXIT_FAILURE);
		}
	}

	lua_handlers_init();

	if(!get_hw_revision(&swcfg.hw))
		INFO("Running on %s Revision %s", swcfg.hw.boardname, swcfg.hw.revision);

	print_registered_handlers();
	if (swcfg.globals.syslog_enabled) {
		if (syslog_init()) {
			ERROR("failed to initialize syslog notifier");
		}
	}

	if (opt_e) {
		if (parse_image_selector(software_select, &swcfg)) {
			fprintf(stderr, "Error: Incorrect select option format.\n");
			exit(EXIT_FAILURE);
		}
		fprintf(stderr, "software set: %s mode: %s\n",
			swcfg.globals.default_software_set, swcfg.globals.default_running_mode);
	}

	/* Read sw-versions */
	get_sw_versions(cfgfname, &swcfg);

	/*
	 *  Do not start daemon if just a check is required
	 *  SWUpdate will exit after the check
	 */
	if (!opt_c) {
		network_daemon = start_thread(network_initializer, &swcfg);

		start_thread(progress_bar_thread, NULL);

		/* Start embedded web server */
#if defined(CONFIG_MONGOOSE)
		if (opt_w) {
			start_subprocess(SOURCE_WEBSERVER, "webserver",
					 cfgfname, ac, av,
					 start_mongoose);
			freeargs(av);
		}
#endif

#if defined(CONFIG_SURICATTA)
		if (opt_u) {
			start_subprocess(SOURCE_SURICATTA, "suricatta",
					 cfgfname, argcount,
					 argvalues, start_suricatta);

			freeargs(argvalues);
		}
#endif

#ifdef CONFIG_DOWNLOAD
		if (opt_d) {
			start_subprocess(SOURCE_DOWNLOADER, "download",
					 cfgfname, dwlac,
					 dwlav, start_download);
			freeargs(dwlav);
		}
#endif

		/*
		 * Start all processes added in the config file
		 */
		struct extproc *proc;

		LIST_FOREACH(proc, &swcfg.extprocs, next) {
			dwlav = splitargs(proc->exec, &dwlac);

			dwlav[dwlac] = NULL;

			start_subprocess_from_file(SOURCE_UNKNOWN, proc->name,
						   cfgfname, dwlac,
						   dwlav, dwlav[0]);

			freeargs(dwlav);
		}
	}

	if (opt_i) {

		result = install_from_file(fname, opt_c);
		switch (result) {
		case EXIT_FAILURE:
			if (swcfg.bootloader_transaction_marker) {
				save_state_string((char*)BOOTVAR_TRANSACTION, STATE_FAILED);
			}
			break;
		case EXIT_SUCCESS:
			notify(SUCCESS, 0, INFOLEVEL, NULL);
			if (postupdate(&swcfg, NULL) != 0) {
				ERROR("Post-update command execution failed.");
			}
			break;
		}
		cleanup_files(&swcfg);
	}

#ifdef CONFIG_SYSTEMD
	if (sd_booted()) {
		sd_notify(0, "READY=1");
	}
#endif

	/*
	 * Install a handler for SIGTERM that cancels
	 * the network_daemon thread to allow atexit()
	 * registered functions to run.
	 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigterm_handler;
	sigaction(SIGTERM, &sa, NULL);

	/*
	 * Go into supervisor loop
	 */
	if (!opt_c && !opt_i)
		pthread_join(network_daemon, NULL);

	return result;
}
