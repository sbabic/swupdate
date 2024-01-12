/*
 * (C) Copyright 2015
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <json-c/json.h>
#include <signal.h>

#include "util.h"
#include "network_ipc.h"
#include "download_interface.h"
#include "server_utils.h"
#include "channel.h"
#include "channel_curl.h"
#include "parselib.h"
#include "swupdate_settings.h"
#include "pctl.h"

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
    {"retrywait", required_argument, NULL, 'w'},
    {"timeout", required_argument, NULL, 't'},
    {"authentication", required_argument, NULL, 'a'},
    {NULL, 0, NULL, 0}};

static channel_data_t channel_options = {
	.source = SOURCE_DOWNLOADER,
	.debug = false,
	.retries = DL_DEFAULT_RETRIES,
	.retry_sleep = CHANNEL_DEFAULT_RESUME_DELAY,
	.low_speed_timeout = DL_LOWSPEED_TIME,
	.headers_to_send = NULL,
	.max_download_speed = 0, /* Unlimited download speed is default. */
	.noipc = false,
	.range = NULL,
	.headers = NULL,
};

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

	if (result != FAILURE) {
		ipc_message msg;
		msg.data.procmsg.len = 0;
		if (ipc_postupdate(&msg) != 0 || msg.type != ACK) {
			result = FAILURE;
		}
	}

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
	get_field(LIBCFG_PARSER, elem, "retrywait",
		&opt->retry_sleep);
	get_field(LIBCFG_PARSER, elem, "timeout",
		&opt->low_speed_timeout);

	return 0;
}

static server_op_res_t download_server_ipc(int fd)
{
	ipc_message msg;
	server_op_res_t result = SERVER_OK;
	int ret;
	struct json_object *json_root;
	json_object *json_data;


	ret = read(fd, &msg, sizeof(msg));
	if (ret != sizeof(msg))
		return SERVER_EERR;
	switch (msg.data.procmsg.cmd) {
	case CMD_SET_DOWNLOAD_URL:
		json_root = server_tokenize_msg(msg.data.procmsg.buf,
					sizeof(msg.data.procmsg.buf));
		if (!json_root) {
			ERROR("Wrong JSON message, see documentation");
			result = SERVER_EERR;
			break;
		}
		json_data = json_get_path_key(json_root, (const char *[]){"url", NULL});
		if (!json_data) {
			ERROR("URL is mandatory, no URL found");
			result = SERVER_EERR;
			break;
		}
		channel_options.url = strdup(json_object_get_string(json_data));

		/*
		 * check for authentication
		 */
		json_data = json_get_path_key(json_root, (const char *[]){"userpassword", NULL});
		if (json_data) {
			/*
			 * Credentials are in the curl format, user:password, see CURLOPT_USERPWD
			 */
			channel_options.auth = strdup(json_object_get_string(json_data));
		}
		break;
	default:
		result = SERVER_EERR;
		break;
	}

	msg.data.procmsg.len = 0;
	if (result == SERVER_EERR) {
		msg.type = NACK;
	} else
		msg.type = ACK;

	/*
	 * first send the answer, then blocks until update has finished
	 */
	if (write(fd, &msg, sizeof(msg)) != sizeof(msg)) {
		TRACE("IPC ERROR: sending back msg");
	}

	if (result == SERVER_OK) {
		RECOVERY_STATUS update_result = download_from_url(&channel_options);

		if (channel_options.url != NULL) {
			free(channel_options.url);
		}
		if (channel_options.auth != NULL) {
			free(channel_options.auth);
		}
		channel_options.url = NULL;
		channel_options.auth = NULL;

		result = update_result == SUCCESS ? SERVER_OK : SERVER_EERR;
	}
	/* Send ipc back */
	return result;
}

void download_print_help(void)
{
	fprintf(
	    stdout,
	    "\tdownload arguments (mandatory arguments are marked with '*'):\n"
	    "\t  -u, --url <url>      * <url> is a link to the .swu update image\n"
	    "\t  -r, --retries          number of retries (resumed download) if connection\n"
	    "\t                         is broken (0 means indefinitely retries) (default: %d)\n"
	    "\t  -w, --retrywait      timeout to wait before retrying retries (default: %d)\n"
	    "\t  -t, --timeout          timeout to check if a connection is lost (default: %d)\n"
	    "\t  -a, --authentication   authentication information as username:password\n",
	    DL_DEFAULT_RETRIES, DL_LOWSPEED_TIME, CHANNEL_DEFAULT_RESUME_DELAY);
}

int start_download_server(const char *fname, int argc, char *argv[])
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
	while ((choice = getopt_long(argc, argv, "t:u:w:r:a:",
				     long_options, NULL)) != -1) {
		switch (choice) {
		case 't':
			channel_options.low_speed_timeout = strtoul(optarg, NULL, 10);
			break;
		case 'w':
			channel_options.retry_sleep = strtoul(optarg, NULL, 10);
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

	/*
	 * if URL is passed, this is a one time step
	 * and update is started automatically. SWUpdate will
	 * exit after the update
	 */
	if (channel_options.url) {
		RECOVERY_STATUS result = download_from_url(&channel_options);

		if (channel_options.url != NULL) {
			free(channel_options.url);
		}
		if (channel_options.auth != NULL) {
			free(channel_options.auth);
		}
		exit(result == SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE);
	}

	/*
	 * Loop waiting for IPC connection.
	 * Ther eis no other running thread in this process, so
	 * it is safe to call ipc_thread_fn() directly without spawning
	 * a new thread. Function dows not return
	 */
	signal(SIGPIPE, SIG_IGN);
	ipc_thread_fn(download_server_ipc);

	/*
	 * function does not return
	 */

	return SERVER_OK;
}
