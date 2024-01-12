/*
 * (C) Copyright 2012-2016
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
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
#include "network_utils.h"
#include "sslapi.h"
#include "suricatta/suricatta.h"
#include "delta_process.h"
#include "progress.h"
#include "parselib.h"
#include "swupdate_settings.h"
#include "pctl.h"
#include "state.h"
#include "bootloader.h"
#include "versions.h"
#include "hw-compatibility.h"
#include "swupdate_vars.h"

#ifdef CONFIG_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#define MODULE_NAME	"swupdate"

static pthread_t network_daemon;

/* Tree derived from the configuration file */
static struct swupdate_cfg swcfg;

int loglevel = ERRORLEVEL;
int exit_code = EXIT_SUCCESS;

#ifdef CONFIG_MTD
/* Global MTD configuration */
static struct flash_description flashdesc;

struct flash_description *get_flash_info(void) {
	return &flashdesc;
}
#endif

static struct option long_options[] = {
	{"accepted-select", required_argument, NULL, 'q'},
#ifdef CONFIG_UBIATTACH
	{"blacklist", required_argument, NULL, 'b'},
#endif
	{"check", no_argument, NULL, 'c'},
#ifdef CONFIG_DOWNLOAD
	{"download", required_argument, NULL, 'd'},
#endif
	{"dry-run", no_argument, NULL, 'n'},
	{"file", required_argument, NULL, 'f'},
	{"get-root", no_argument, NULL, 'g'},
	{"help", no_argument, NULL, 'h'},
#ifdef CONFIG_HW_COMPATIBILITY
	{"hwrevision", required_argument, NULL, 'H'},
#endif
	{"image", required_argument, NULL, 'i'},
#ifdef CONFIG_SIGNED_IMAGES
	{"key", required_argument, NULL, 'k'},
	{"ca-path", required_argument, NULL, 'k'},
	{"cert-purpose", required_argument, NULL, '1'},
#if defined(CONFIG_SIGALG_CMS) && !defined(CONFIG_SSL_IMPL_WOLFSSL)
	{"forced-signer-name", required_argument, NULL, '2'},
#endif
#endif
#ifdef CONFIG_ENCRYPTED_IMAGES
	{"key-aes", required_argument, NULL, 'K'},
#endif
	{"loglevel", required_argument, NULL, 'l'},
	{"max-version", required_argument, NULL, '3'},
	{"no-downgrading", required_argument, NULL, 'N'},
	{"no-reinstalling", required_argument, NULL, 'R'},
	{"no-state-marker", no_argument, NULL, 'm'},
	{"no-transaction-marker", no_argument, NULL, 'M'},
	{"output", required_argument, NULL, 'o'},
	{"preupdate", required_argument, NULL, 'P'},
	{"postupdate", required_argument, NULL, 'p'},
	{"select", required_argument, NULL, 'e'},
#ifdef CONFIG_SURICATTA
	{"suricatta", required_argument, NULL, 'u'},
#endif
	{"syslog", no_argument, NULL, 'L' },
	{"verbose", no_argument, NULL, 'v'},
	{"version", no_argument, NULL, '0'},
#ifdef CONFIG_WEBSERVER
	{"webserver", required_argument, NULL, 'w'},
#endif
	{"bootloader", required_argument, NULL, 'B'},
	{NULL, 0, NULL, 0}
};

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
		" -B, --bootloader               : bootloader interface (default: " PREPROCVALUE(BOOTLOADER_DEFAULT) ")\n"
		" -p, --postupdate               : execute post-update command\n"
		" -P, --preupdate                : execute pre-update command\n"
		" -e, --select <software>,<mode> : Select software images set and source\n"
		"                                  Ex.: stable,main\n"
		" -g, --get-root                 : detect and print the root device and exit\n" 
		" -q, --accepted-select\n"
		"            <software>,<mode>   : List for software images set and source\n"
		"                                  that are accepted via IPC\n"
		"                                  Ex.: stable,main\n"
		"                                  it can be set multiple times\n"
		" -i, --image <filename>         : Software to be installed\n"
		" -l, --loglevel <level>         : logging level\n"
		" -L, --syslog                   : enable syslog logger\n"
#ifdef CONFIG_SIGNED_IMAGES
#ifndef CONFIG_SIGALG_GPG
		" -k, --key <public key file>    : file with public key to verify images\n"
		"     --cert-purpose <purpose>   : set expected certificate purpose\n"
		"                                  [emailProtection|codeSigning] (default: emailProtection)\n"
#if defined(CONFIG_SIGALG_CMS) && !defined(CONFIG_SSL_IMPL_WOLFSSL)
		"     --forced-signer-name <cn>  : set expected common name of signer certificate\n"
#endif
		"     --ca-path                  : path to the Certificate Authority (PEM)\n"
#endif
#endif
#ifdef CONFIG_ENCRYPTED_IMAGES
		" -K, --key-aes <key file>       : the file contains the symmetric key to be used\n"
		"                                  to decrypt images\n"
#endif
		" -n, --dry-run                  : run SWUpdate without installing the software\n"
		" -N, --no-downgrading <version> : not install a release older as <version>\n"
		" -R, --no-reinstalling <version>: not install a release same as <version>\n"
		"     --max-version     <version>: not install a release bigger as <version>\n"
		" -M, --no-transaction-marker    : disable setting bootloader transaction marker\n"
		" -m, --no-state-marker          : disable setting update state in bootloader\n"
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

static int parse_image_selector(const char *selector, struct swupdate_cfg *sw)
{
	char *pos;

	DEBUG("Parsing selector: %s", selector);
	pos = strchr(selector, ',');
	if (pos == NULL) {
		ERROR("Incorrect select option format: %s", selector);
		return -EINVAL;
	}

	*pos = '\0';

	/*
	 * the runtime copy in swcfg can be overloaded by IPC,
	 * so maintain a copy to restore it after an update
	 */
	strlcpy(sw->parms.software_set, selector, sizeof(sw->parms.software_set));
	/* pos + 1 will either be NULL or valid text */
	strlcpy(sw->parms.running_mode, pos + 1, sizeof(sw->parms.running_mode));

	if (strlen(sw->parms.software_set) == 0 || strlen(sw->parms.running_mode) == 0)
		return -EINVAL;

	return 0;
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
	sw->cert_purpose = SSL_PURPOSE_DEFAULT;

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
				"bootloader", tmp);
	if (tmp[0] != '\0') {
		if (set_bootloader(tmp) != 0) {
			ERROR("Bootloader interface '%s' could not be initialized.", tmp);
			exit(EXIT_FAILURE);
		}
		tmp[0] = '\0';
	}
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"public-key-file", sw->publickeyfname);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"ca-path", sw->publickeyfname);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"aes-key-file", sw->aeskeyfname);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"mtd-blacklist", sw->mtdblacklist);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"postupdatecmd", sw->postupdatecmd);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"preupdatecmd", sw->preupdatecmd);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"namespace-vars", sw->namespace_for_vars);
	if (strlen(sw->namespace_for_vars)) {
		if (!swupdate_set_default_namespace(sw->namespace_for_vars))
			WARN("Default Namaspace for SWUpdate vars cannot be set, possible side-effects");
	}

	get_field(LIBCFG_PARSER, elem, "verbose", &sw->verbose);
	get_field(LIBCFG_PARSER, elem, "loglevel", &sw->loglevel);
	get_field(LIBCFG_PARSER, elem, "syslog", &sw->syslog_enabled);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"no-downgrading", sw->minimum_version);
	tmp[0] = '\0';
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"fwenv-config-location", tmp);
	if (strlen(tmp)) {
		set_fwenv_config(tmp);
		tmp[0] = '\0';
	}
	if (strlen(sw->minimum_version))
		sw->no_downgrading = true;
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"max-version", sw->maximum_version);
	if (strlen(sw->maximum_version))
		sw->check_max_version = true;
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"no-reinstalling", sw->current_version);
	if (strlen(sw->current_version))
		sw->no_reinstalling = true;
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"cert-purpose", tmp);
	if (tmp[0] != '\0')
		sw->cert_purpose = parse_cert_purpose(tmp);
	GET_FIELD_STRING(LIBCFG_PARSER, elem, "forced-signer-name",
				sw->forced_signer_name);

	char software_select[SWUPDATE_GENERAL_STRING_SIZE] = "";
	GET_FIELD_STRING(LIBCFG_PARSER, elem, "select", software_select);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"gpg-home-dir", sw->gpg_home_directory);
	GET_FIELD_STRING(LIBCFG_PARSER, elem,
				"gpgme-protocol", sw->gpgme_protocol);
	if (software_select[0] != '\0') {
		/* by convention, errors in a configuration section are ignored */
		(void)parse_image_selector(software_select, sw);
	}

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
		if (!proc) {
			ERROR("OOM reading process settings, exiting...");
			exit(EXIT_FAILURE);
		}

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
	bool opt_c = false;
	char image_url[MAX_URL];
	char main_options[256];
	unsigned int public_key_mandatory = 0;
	struct sigaction sa;
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
	strcpy(main_options, "vhni:e:gq:l:Lcf:p:P:o:N:R:MmB:");
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
#ifndef CONFIG_SIGALG_GPG
	strcat(main_options, "k:");
	public_key_mandatory = 1;
#endif
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
		char *root;
		case 'f':
			cfgfname = sdup(optarg);
			break;
		case 'g':
			root = get_root_device();
			if (root) {
				printf("%s\n", root);
				free(root);
			}
			exit(EXIT_SUCCESS);
			break;
		case 'l':
			loglevel = strtoul(optarg, NULL, 10);
			break;
		case 'v':
			loglevel = LASTLOGLEVEL;
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
	swupdate_cfg_handle handle;
	swupdate_cfg_init(&handle);
	if (cfgfname != NULL) {
		int ret = swupdate_cfg_read_file(&handle, cfgfname);

		/*
		 * 'globals' section is mandatory if configuration file is specified.
		 */
		if (ret == 0) {
			ret = read_module_settings(&handle, "globals", read_globals_settings, &swcfg);
		}
		if (ret != 0) {
			/*
			 * Exit on -ENODATA or -EINVAL errors.
			 */
			fprintf(stderr,
			    "Error parsing configuration file: %s, exiting.\n",
			    ret == -ENODATA ? "'globals' section missing"
					    : "cannot read");
			swupdate_cfg_destroy(&handle);
			exit(EXIT_FAILURE);
		}

		loglevel = swcfg.verbose ? LASTLOGLEVEL : swcfg.loglevel;

		/*
		 * The following sections are optional, hence -ENODATA error code is
		 * ignored if the section is not found. -EINVAL will not happen here.
		 */
		(void)read_module_settings(&handle, "logcolors", read_console_settings, &swcfg);
		(void)read_module_settings(&handle, "processes", read_processes_settings, &swcfg);
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
			loglevel = LASTLOGLEVEL;
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
		case 'B':
			if (set_bootloader(optarg) != 0) {
				ERROR("Bootloader interface '%s' could not be initialized.", optarg);
				print_registered_bootloaders();
				exit(EXIT_FAILURE);
			}
			break;
		case 'l':
			loglevel = strtoul(optarg, NULL, 10);
			break;
		case 'n':
			swcfg.parms.dry_run = true;
			break;
		case 'L':
			swcfg.syslog_enabled = true;
			break;
		case 'k':
			strlcpy(swcfg.publickeyfname,
				optarg,
			       	sizeof(swcfg.publickeyfname));
			break;
		case '1':
			swcfg.cert_purpose = parse_cert_purpose(optarg);
			break;
		case '2':
			strlcpy(swcfg.forced_signer_name, optarg,
				sizeof(swcfg.forced_signer_name));
			break;
		case '3':
			swcfg.check_max_version = true;
			strlcpy(swcfg.maximum_version, optarg,
				sizeof(swcfg.maximum_version));
			break;
#ifdef CONFIG_ENCRYPTED_IMAGES
		case 'K':
			strlcpy(swcfg.aeskeyfname,
				optarg,
			       	sizeof(swcfg.aeskeyfname));
			break;
#endif
		case 'N':
			swcfg.no_downgrading = true;
			strlcpy(swcfg.minimum_version, optarg,
				sizeof(swcfg.minimum_version));
			break;
		case 'R':
			swcfg.no_reinstalling = true;
			strlcpy(swcfg.current_version, optarg,
				sizeof(swcfg.current_version));
			break;
		case 'M':
			swcfg.no_transaction_marker = true;
			TRACE("transaction_marker globally disabled");
			break;
		case 'm':
			swcfg.no_state_marker = true;
			TRACE("state_marker globally disabled");
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
			opt_c = true;
			break;
		case 'p':
			strlcpy(swcfg.postupdatecmd, optarg,
				sizeof(swcfg.postupdatecmd));
			break;
		case 'P':
			strlcpy(swcfg.preupdatecmd, optarg,
				sizeof(swcfg.preupdatecmd));
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
	if (public_key_mandatory && !strlen(swcfg.publickeyfname)) {
		fprintf(stderr,
			 "Error: SWUpdate is built for signed images, provide a public key file.\n");
		exit(EXIT_FAILURE);
	}

#ifdef CONFIG_SIGALG_GPG
	if (!strlen(swcfg.gpg_home_directory)) {
		fprintf(stderr,
			 "Error: SWUpdate is built for signed images, provide a GnuPG home directory.\n");
		exit(EXIT_FAILURE);
	}
	if (!strlen(swcfg.gpgme_protocol)) {
		fprintf(stderr,
			"Error: SWUpdate is built for signed images, please specify GnuPG protocol.\n");
		exit(EXIT_FAILURE);
	}
#endif

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

	if (strlen(swcfg.publickeyfname) || strlen(swcfg.gpg_home_directory)) {
		if (swupdate_dgst_init(&swcfg, swcfg.publickeyfname)) {
			fprintf(stderr,
				 "Error: Crypto cannot be initialized.\n");
			exit(EXIT_FAILURE);
		}
	}

	printf("%s\n", BANNER);
	printf("Licensed under GPLv2. See source distribution for detailed "
		"copyright notices.\n\n");

	print_registered_bootloaders();
	if (!get_bootloader()) {
		if (set_bootloader(PREPROCVALUE(BOOTLOADER_DEFAULT)) != 0) {
			ERROR("Default bootloader interface '" PREPROCVALUE(
			    BOOTLOADER_DEFAULT) "' couldn't be loaded.");
			INFO("Check that the bootloader interface shared library is present.");
			INFO("Or chose another bootloader interface by supplying -B <loader>.");
			exit(EXIT_FAILURE);
		}
		INFO("Using default bootloader interface: " PREPROCVALUE(BOOTLOADER_DEFAULT));
	} else {
		INFO("Using bootloader interface: %s", get_bootloader());
	}

	/*
	 * Install a child handler to check if a subprocess
	 * dies
	 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigchld_handler;
	sigaction(SIGCHLD, &sa, NULL);
#ifdef CONFIG_UBIATTACH
	if (strlen(swcfg.mtdblacklist))
		mtd_set_ubiblacklist(swcfg.mtdblacklist);
#endif

	/*
	 * If an AES key is passed, load it to allow
	 * to decrypt images
	 */
	if (strlen(swcfg.aeskeyfname)) {
		if (load_decryption_key(swcfg.aeskeyfname)) {
			fprintf(stderr,
				"Error: Key file does not contain a valid AES key.\n");
			exit(EXIT_FAILURE);
		}
	}

	lua_handlers_init();

	if(!get_hw_revision(&swcfg.hw))
		INFO("Running on %s Revision %s", swcfg.hw.boardname, swcfg.hw.revision);

	print_registered_handlers();
	if (swcfg.syslog_enabled) {
		if (syslog_init()) {
			ERROR("failed to initialize syslog notifier");
		}
	}

	if (opt_e) {
		if (parse_image_selector(software_select, &swcfg)) {
			fprintf(stderr, "Error: Incorrect select option format.\n");
			exit(EXIT_FAILURE);
		}
	}

	/* check if software_set or running_mode was parsed and log both values */
	if (swcfg.parms.software_set[0] != '\0' || swcfg.parms.running_mode[0] != '\0') {
		INFO("software set: %s mode: %s", swcfg.parms.software_set,
			swcfg.parms.running_mode);
	}

	/* Read sw-versions */
	get_sw_versions(&handle, &swcfg);

	/*
	 *  Start daemon if just a check is required
	 *  SWUpdate will exit after the check
	 */
	if(init_socket_unlink_handler() != 0){
		TRACE("Cannot setup socket cleanup on exit, sockets won't be unlinked.");
	}
	network_daemon = start_thread(network_initializer, &swcfg);

	start_thread(progress_bar_thread, NULL);

	/* wait for threads to be done before starting children */
	wait_threads_ready();

	/* Start embedded web server */
#if defined(CONFIG_MONGOOSE)
	if (opt_w) {
		uid_t uid;
		gid_t gid;
		read_settings_user_id(&handle, "webserver", &uid, &gid);
		start_subprocess(SOURCE_WEBSERVER, "webserver", uid, gid,
				 cfgfname, ac, av,
				 start_mongoose);
		freeargs(av);
	}
#endif

#if defined(CONFIG_SURICATTA)
	if (opt_u) {
		uid_t uid;
		gid_t gid;
		read_settings_user_id(&handle, "suricatta", &uid, &gid);
		start_subprocess(SOURCE_SURICATTA, "suricatta", uid, gid,
				 cfgfname, argcount,
				 argvalues, start_suricatta);

		freeargs(argvalues);
	}
#endif

#ifdef CONFIG_DOWNLOAD
	if (opt_d) {
		uid_t uid;
		gid_t gid;
		read_settings_user_id(&handle, "download", &uid, &gid);
		start_subprocess(SOURCE_DOWNLOADER, "download", uid, gid,
				 cfgfname, dwlac,
				 dwlav, start_download_server);
		freeargs(dwlav);
	}
#endif
#if defined(CONFIG_DELTA)
	{
		uid_t uid;
		gid_t gid;
		read_settings_user_id(&handle, "download", &uid, &gid);
		start_subprocess(SOURCE_CHUNKS_DOWNLOADER, "chunks_downloader", uid, gid,
				cfgfname, 0, NULL,
				start_delta_downloader);
	}
#endif


	/*
	 * Start all processes added in the config file
	 */
	struct extproc *proc;

	LIST_FOREACH(proc, &swcfg.extprocs, next) {
		dwlav = splitargs(proc->exec, &dwlac);

		dwlav[dwlac] = NULL;

		uid_t uid;
		gid_t gid;
		read_settings_user_id(&handle, proc->name, &uid, &gid);
		start_subprocess_from_file(SOURCE_UNKNOWN, proc->name, uid, gid,
					   cfgfname, dwlac,
					   dwlav, dwlav[0]);

		freeargs(dwlav);
	}

	if (opt_i) {
		exit_code = install_from_file(fname, opt_c);
	}

#ifdef CONFIG_SYSTEMD
	sd_notify(0, "READY=1");
#endif

	/*
	 * Install a handler for SIGTERM that cancels
	 * the network_daemon thread to allow atexit()
	 * registered functions to run.
	 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigterm_handler;
	sigaction(SIGTERM, &sa, NULL);

	swupdate_cfg_destroy(&handle);

	/*
	 * Go into supervisor loop
	 */
	if (!opt_c && !opt_i)
		pthread_join(network_daemon, NULL);

	return exit_code;
}
