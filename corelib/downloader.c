/*
 * (C) Copyright 2015
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdlib.h>
#include <errno.h>
#include <getopt.h>

#include "util.h"
#include "network_ipc.h"
#include "download_interface.h"
#include "channel.h"
#include "channel_curl.h"
#include "parselib.h"
#include "swupdate_settings.h"

/*
 * Number of seconds while below low speed
 * limit before aborting. It can be overwritten
 * by -t command line flag.
 */
#define DL_LOWSPEED_TIME	300

#define DL_DEFAULT_RETRIES	3

static struct option long_options[] = {
    {"url", required_argument, NULL, 'u'},
    {"retries", required_argument, NULL, 'r'},
    {"timeout", required_argument, NULL, 't'},
    {"authentication", required_argument, NULL, 'a'},
    {NULL, 0, NULL, 0}};

/*
 * This provides a pull from an external server
 * It is not thought to work with local (file://)
 * files. For that, the -i option is used.
 */
static RECOVERY_STATUS download_from_url(channel_data_t* channel_data)
{
	channel_t *channel = channel_new();
	if (channel->open(channel, channel_data) != CHANNEL_OK) {
		free(channel);
		return FAILURE;
	}

	TRACE("Image download started : %s", channel_data->url);

	RECOVERY_STATUS result = SUCCESS;
	channel_data->source = SOURCE_DOWNLOADER;
	channel_op_res_t chanresult = channel->get_file(channel, channel_data);
	if (chanresult != CHANNEL_OK) {
		result = FAILURE;
	}
	if (ipc_wait_for_complete(NULL) != SUCCESS) {
		result = FAILURE;
	}
	channel->close(channel);
	free(channel);
	return result;
}

static int download_settings(void *elem, void  __attribute__ ((__unused__)) *data)
{
	channel_data_t *opt = (channel_data_t *)data;
	char tmp[SWUPDATE_GENERAL_STRING_SIZE];

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "url", tmp);
	if (strlen(tmp)) {
		SETSTRING(opt->url, tmp);
	}

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "authentication", tmp);
	if (strlen(tmp)) {
		SETSTRING(opt->auth, tmp);
	} else {
		opt->auth = NULL;
	}

	get_field(LIBCFG_PARSER, elem, "retries",
		&opt->retries);
	get_field(LIBCFG_PARSER, elem, "timeout",
		&opt->low_speed_timeout);

	return 0;
}

void download_print_help(void)
{
	fprintf(
	    stdout,
	    "\tdownload arguments (mandatory arguments are marked with '*'):\n"
	    "\t  -u, --url <url>      * <url> is a link to the .swu update image\n"
	    "\t  -r, --retries          number of retries (resumed download) if connection\n"
	    "\t                         is broken (0 means indefinitely retries) (default: %d)\n"
	    "\t  -t, --timeout          timeout to check if a connection is lost (default: %d)\n"
	    "\t  -a, --authentication   authentication information as username:password\n",
	    DL_DEFAULT_RETRIES, DL_LOWSPEED_TIME);
}

static channel_data_t channel_options = {
	.source = SOURCE_DOWNLOADER,
	.debug = false,
	.retries = DL_DEFAULT_RETRIES,
	.low_speed_timeout = DL_LOWSPEED_TIME
};

int start_download(const char *fname, int argc, char *argv[])
{
	if (fname) {
		swupdate_cfg_handle handle;
		swupdate_cfg_init(&handle);
		if (swupdate_cfg_read_file(&handle, fname) == 0) {
			read_module_settings(&handle, "download", download_settings, &channel_options);
		}
		swupdate_cfg_destroy(&handle);
	}

	/* reset to optind=1 to parse download's argument vector */
	optind = 1;
	int choice = 0;
	while ((choice = getopt_long(argc, argv, "t:u:r:a:",
				     long_options, NULL)) != -1) {
		switch (choice) {
		case 't':
			channel_options.low_speed_timeout = strtoul(optarg, NULL, 10);
			break;
		case 'u':
			SETSTRING(channel_options.url, optarg);
			break;
		case 'a':
			SETSTRING(channel_options.auth, optarg);
			break;
		case 'r':
			channel_options.retries = strtoul(optarg, NULL, 10);
			break;
		case '?':
		default:
			return -EINVAL;
		}
	}

	RECOVERY_STATUS result = download_from_url(&channel_options);
	if (result != FAILURE) {
		ipc_message msg;
		if (ipc_postupdate(&msg) != 0) {
			result = FAILURE;
		} else {
			result = msg.type == ACK ? result : FAILURE;
		}
	}

	if (channel_options.url != NULL) {
		free(channel_options.url);
	}
	if (channel_options.auth != NULL) {
		free(channel_options.auth);
	}

	exit(result == SUCCESS ? EXIT_SUCCESS : result);
}
