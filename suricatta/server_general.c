/*
 * (C) Copyright 2018
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

/*
 * This implements a very simple interface where the API is
 * realised with HTTP return codes.
 * See documentation for more details
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <assert.h>
#include <errno.h>
#include <generated/autoconf.h>
#include <util.h>
#include <network_ipc.h>
#include <sys/time.h>
#include <swupdate_status.h>
#include "suricatta/suricatta.h"
#include "suricatta_private.h"
#include "parselib.h"
#include "channel.h"
#include <curl/curl.h>
#include "channel_curl.h"
#include "state.h"
#include "parselib.h"
#include "swupdate_settings.h"
#include "swupdate_dict.h"
#include "server_general.h"
#include <progress_ipc.h>
#include <pctl.h>
#include <pthread.h>

/* Prototypes for "public" functions */
void server_print_help(void);
server_op_res_t server_has_pending_action(int *action_id);
server_op_res_t server_stop(void);
server_op_res_t server_ipc(ipc_message *msg);
server_op_res_t server_start(char *fname, int argc, char *argv[]);
server_op_res_t server_install_update(void);
server_op_res_t server_send_target_data(void);
unsigned int server_get_polling_interval(void);

/*
 * This is a "specialized" map_http_retcode() because
 * the return codes are interpreted
 *
 * 404 = No update
 * 302 = New Software available
 */
static server_op_res_t map_http_retcode(channel_op_res_t response);

static struct option long_options[] = {
    {"url", required_argument, NULL, 'u'},
    {"logurl", required_argument, NULL, 'l'},
    {"polldelay", required_argument, NULL, 'p'},
    {"retry", required_argument, NULL, 'r'},
    {"retrywait", required_argument, NULL, 'w'},
    {NULL, 0, NULL, 0}};

static unsigned short mandatory_argument_count = 0;

/*
 * Structure passed to progress thread
 * with configuration (URL to LOG, setup)
 */
typedef struct {
	char *url;
	struct dict *identify;
	char *fname;
} server_progress_data;
static server_progress_data progdata;

/*
 * These are used to check if all mandatory fields
 * are set
 */
#define URL_BIT		4
#define ALL_MANDATORY_SET	(URL_BIT)

/*
 * Defibe max size for a log message
 */
#define MAX_LOG_SIZE 1024

extern channel_op_res_t channel_curl_init(void);

server_general_t server_general = {.url = NULL,
				   .polling_interval = 30,
				   .debug = false,
				   .channel = NULL};

static channel_data_t channel_data_defaults = {.debug = false,
					       .source = SOURCE_SURICATTA,
					       .retries = CHANNEL_DEFAULT_RESUME_TRIES,
					       .retry_sleep =
						   CHANNEL_DEFAULT_RESUME_DELAY,
#ifdef CONFIG_SURICATTA_SSL
					       .usessl = true,
#endif
					       .format = CHANNEL_PARSE_RAW,
					       .nocheckanswer = true,
					       .nofollow = true,
					       .strictssl = true};

/*
 * This backend has no communication during an update.
 * If an update is in progress, it cannot be stopped
 */
static int server_general_status_callback(ipc_message __attribute__ ((__unused__)) *msg)
{
	return 0;
}

/*
 * Callback used by reading setup from configuration file
 * @settings = node in cfg
 * @data = pointer to a dictionary list where the callback stores
 *         the formatting fields for each event
 */
static int server_logevent_settings(void *settings, void  *data)
{
	void *elem;
	int count, i;
	struct dict *events;
	char event[80], fmt[80];

	if (!data)
		return -EINVAL;

	events = (struct dict *)data;

	count = get_array_length(LIBCFG_PARSER, settings);

	for(i = 0; i < count; ++i) {
		elem = get_elem_from_idx(LIBCFG_PARSER, settings, i);
		if (!elem)
			continue;

		/*
		 * both event and format must be set, else
		 * the setting is malformed and then ignored
		 */
		if(!(exist_field_string(LIBCFG_PARSER, elem, "event")))
			continue;
		if(!(exist_field_string(LIBCFG_PARSER, elem, "format")))
			continue;

		GET_FIELD_STRING(LIBCFG_PARSER, elem, "event", event);
		GET_FIELD_STRING(LIBCFG_PARSER, elem, "format", fmt);
		TRACE("event: %s, format: %s", event, fmt);
		dict_set_value(events, event, fmt);
	}

	return 0;
}

/*
 * The LOG must be formatted before sending to the server
 * It is a CSV formatted text with fields set from configuration
 * file. Each field is lookuped into the identity list and
 * replaced if a match is found with the real value.
 * If field has the special value "date", the current local time
 * formatted as specified in RFC 2822 is sent.
 * If no match is found, the field is interpreted as constant and
 * sent as it is.
 */
static char *server_format_log(const char *event, struct dict *fmtevents,
				struct dict *identify)
{
	char *fmt, *tmp;
	char *fdate;
	char *token;
	char *saveptr = NULL;

	if (!fmtevents || !event)
		return NULL;

	if ( !(tmp = dict_get_value(fmtevents, event)))
		return NULL;

	char *log = (char *)calloc(1, MAX_LOG_SIZE);
	if (!log)
		return NULL;

	fmt = strdup(tmp);
	token = strtok_r(fmt, ",", &saveptr);

	fdate = swupdate_time_iso8601();

	while (token) {
		char *field;

		/*
		 * The field "date" is interpreted as localtime
		 */
		if (!strcmp(token, "date"))
			field = fdate;
		else
			field = dict_get_value(identify, token);

		if (strlen(log) && strlen(log) < MAX_LOG_SIZE)
			strcat(log,",");

		if (!field) {
			strncat(log, token, MAX_LOG_SIZE - strlen(log) - 1);
		} else {
			strncat(log, field, MAX_LOG_SIZE - strlen(log) - 1);
		}
		token = strtok_r(NULL, ",", &saveptr);
	}
	free(fmt);
	free(fdate);
	TRACE("Formatted log: %s", log);

	return log;
}

/*
 * progress thread. This is started to get all
 * changes during an install exactly as a separate process
 * via progress interface.
 * It manages to send the LOG to the server with PUT
 * method
 */

static void *server_progress_thread (void *data)
{
	int progfd = -1;		/* File descriptor progress API */
	struct progress_msg msg;
	RECOVERY_STATUS	status = IDLE;	/* Update Status (Running, Failure) */
	server_progress_data *prog = (server_progress_data *)data;
	channel_t *channel;
	channel_data_t channel_data = channel_data_defaults;
	struct dict fmtevents;
	server_op_res_t result;
	char *logbuffer = NULL;

	LIST_INIT(&fmtevents);

	if (!prog) {
		ERROR("Fatal Error: thread without data !");
	}
	if (!prog->url || !strlen(prog->url)) {
		INFO("No url for logging...no result sent");
		pthread_exit((void *)SERVER_EINIT);
	}

	channel = channel_new();
	if (!channel) {
		ERROR("Cannot get channel for communication");
	}
	if (channel->open(channel, &channel_data) != CHANNEL_OK) {
		ERROR("Cannot open channel for progress thread");
		pthread_exit((void *)SERVER_EINIT);
	}

	if(prog->fname)
		read_module_settings(prog->fname, "gservice.logevent", server_logevent_settings,
					&fmtevents);

	/*
	 * The URL to send log is fixed and part of the configuration
	 */
	channel_data.url = prog->url;

	TRACE("gservice progress thread started, log to \"%s\" !", prog->url);

	while (1) {
		if (progfd < 0) {
			progfd = progress_ipc_connect(true);
		}

		/*
		 * if still fails, try later
		 */
		if (progfd < 0) {
			sleep(1);
			continue;
		}

		if (progress_ipc_receive(&progfd, &msg) == -1) {
			continue;
		}

		/*
		 * Something happens, show the info
		 */
		if ((status == IDLE) && (msg.status != IDLE)) {
			/* New update started */
			logbuffer = server_format_log("started", &fmtevents, prog->identify);

		}

		switch (msg.status) {
		case SUCCESS:
			logbuffer = server_format_log("success", &fmtevents, prog->identify);
			break;

		case FAILURE:
			logbuffer = server_format_log("fail", &fmtevents, prog->identify);
			break;
		case DONE:
			break;
		default:
			break;
		}

		if (logbuffer) {
			channel_data.request_body = logbuffer;
			channel_data.method = CHANNEL_PUT;
			channel_data.content_type = "application/text";
			result = map_channel_retcode(channel->put(channel, (void *)&channel_data));
			if (result != SERVER_OK)
				ERROR("Sending log to server failed !");
			free(logbuffer);
			logbuffer = NULL;
		}
		status = msg.status;
	}

	(void)channel->close(channel);
	pthread_exit((void *)0);
}

static char *server_prepare_query(char *url, struct dict *dict)
{
	int ret;
	struct dict_entry *entry;
	char *qry = NULL;
	char *tmp;
	CURL *curl = curl_easy_init();

	if (!curl)
		return NULL;

	LIST_FOREACH(entry, dict, next) {
		char *key = dict_entry_get_key(entry);
		char *value = dict_entry_get_value(entry);
		char *encoded = curl_easy_escape(curl, value, 0);

		if (!encoded) {
			ERROR("Memory error calling curl_easy_escape");
			if (qry)
				free(qry);
			qry = NULL;
			goto cleanup;
		}

		if (qry) {
			tmp = qry;
			ret = asprintf(&qry, "%s&%s=%s", tmp, key, encoded);
			/*
			 * Free previous instance of the query string
			 */
			free(tmp);
		} else {
			ret = asprintf(&qry, "%s?%s=%s", url, key, encoded);
		}

		/*
		 * Free temporary allocated memory
		 */
		free(encoded);

		if (ret == ENOMEM_ASPRINTF) {
			ERROR("Error generating query for general service");
			qry = NULL;
			goto cleanup;
		}
	}

cleanup:
	curl_easy_cleanup(curl);

	if (!qry)
		qry = url;

	return qry;

}

static server_op_res_t map_http_retcode(channel_op_res_t response)
{
	/*
	 * Check just for library errors
	 */

	switch (response) {
	case CHANNEL_ENONET:
	case CHANNEL_EAGAIN:
		return SERVER_EAGAIN;
	case CHANNEL_EACCES:
		return SERVER_EACCES;
	case CHANNEL_ENOENT:
	case CHANNEL_EIO:
	case CHANNEL_EILSEQ:
	case CHANNEL_ENOMEM:
	case CHANNEL_EINIT:
	case CHANNEL_ELOOP:
		return SERVER_EERR;
	case CHANNEL_EBADMSG:
		return SERVER_EBADMSG;
	case CHANNEL_EREDIRECT:
		return SERVER_UPDATE_AVAILABLE;
	case CHANNEL_ENOTFOUND:
		return SERVER_NO_UPDATE_AVAILABLE;
	case CHANNEL_OK:
		return SERVER_EERR;
	}
	return SERVER_EERR;
}

static server_op_res_t server_set_polling_interval(char *poll)
{
	unsigned long polling_interval = strtoul(poll, NULL, 10);

	server_general.polling_interval =
	    polling_interval == 0 ? CHANNEL_DEFAULT_POLLING_INTERVAL : polling_interval;
	DEBUG("Set polling interval to %ds as announced by server.\n",
	      server_general.polling_interval);

	return SERVER_OK;
}

static server_op_res_t server_get_deployment_info(channel_t *channel, channel_data_t *channel_data)
{
	char *pollstring;
	server_op_res_t result;
	assert(channel != NULL);
	assert(channel_data != NULL);

	/*
	 * Call a function to prepare the URL for getting deployment info
	 * Function allocates memory for URL, it must be freed before
	 * returning
	 */
	channel_data->url= server_prepare_query(server_general.url, &server_general.configdata);

	LIST_INIT(&server_general.httpheaders);
	channel_data->headers = &server_general.httpheaders;

	result = map_http_retcode(channel->get(channel, (void *)channel_data));

	if (channel_data->url != NULL) {
		free(channel_data->url);
	}

	pollstring = dict_get_value(&server_general.httpheaders, "Retry-After");
	if (pollstring) {
		server_set_polling_interval(pollstring);
	}

	dict_drop_db(&server_general.httpheaders);

	return result;
}

server_op_res_t server_has_pending_action(int *action_id)
{

	channel_data_t channel_data = channel_data_defaults;
	server_op_res_t result =
	    server_get_deployment_info(server_general.channel,
				       &channel_data);

	/*
	 * action_id is not used by this server
	 * There is no memory between one call and the next one
	 */
	*action_id = 0;

	if ((result == SERVER_UPDATE_AVAILABLE) &&
	    (get_state() == STATE_INSTALLED)) {
		WARN("An already installed update is pending testing, "
		     "ignoring available update action.");
		INFO("Please restart SWUpdate to report the test results "
		     "upstream.");
		result = SERVER_NO_UPDATE_AVAILABLE;
	}

	return result;
}

server_op_res_t server_send_target_data(void)
{

	return SERVER_OK;
}

unsigned int server_get_polling_interval(void)
{
	return server_general.polling_interval;
}

void server_print_help(void)
{
	fprintf(
	    stdout,
	    "\t  -u, --url         * Host and port of the server instance, "
	    "e.g., localhost:8080\n"
	    "\t  -p, --polldelay     Delay in seconds between two hawkBit "
	    "poll operations (default: %ds).\n"
	    "\t  -r, --retry         Resume and retry interrupted downloads "
	    "(default: %d tries).\n"
	    "\t  -w, --retrywait     Time to wait prior to retry and "
	    "resume a download (default: %ds).\n"
	    "\t  -y, --proxy         Use proxy. Either give proxy URL, else "
	    "{http,all}_proxy env is tried.\n",
	    CHANNEL_DEFAULT_POLLING_INTERVAL, CHANNEL_DEFAULT_RESUME_TRIES,
	    CHANNEL_DEFAULT_RESUME_DELAY);
}

server_op_res_t server_install_update(void)
{
	channel_data_t channel_data = channel_data_defaults;
	server_op_res_t result = SERVER_OK;
	channel_t *channel = server_general.channel;
	char *url = channel->get_redirect_url(channel);

	if (!url)
		return SERVER_EERR;

	channel_data.nofollow = false;
	channel_data.nocheckanswer = false;
	channel_data.checkdwl = NULL;

	channel_data.url = strdup(url);

	channel_op_res_t cresult =
	    channel->get_file(channel, (void *)&channel_data);
	if ((result = map_channel_retcode(cresult)) != SERVER_OK) {
		/* this is called to collect errors */
		ipc_wait_for_complete(server_general_status_callback);
		goto cleanup;
	}

	switch (ipc_wait_for_complete(server_general_status_callback)) {
	case DOWNLOAD:
	case IDLE:
	case START:
	case RUN:
	case SUCCESS:
		result = SERVER_OK;
		break;
	case FAILURE:
		result = SERVER_EERR;
		goto cleanup;
	}

cleanup:
	free(channel_data.url);
	return result;
}

static int server_general_settings(void *elem, void  __attribute__ ((__unused__)) *data)
{
	char tmp[128];

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "url", tmp);
	if (strlen(tmp)) {
		SETSTRING(server_general.url, tmp);
		mandatory_argument_count |= URL_BIT;
	}

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "logurl", tmp);
	if (strlen(tmp)) {
		SETSTRING(server_general.logurl, tmp);
	}

	get_field(LIBCFG_PARSER, elem, "polldelay",
		&server_general.polling_interval);

	suricatta_channel_settings(elem, &channel_data_defaults);

	return 0;
}

server_op_res_t server_start(char *fname, int argc, char *argv[])
{
	int choice = 0;

	LIST_INIT(&server_general.configdata);

	if (fname) {

		read_module_settings(fname, "gservice", server_general_settings,
					NULL);
		read_module_settings(fname, "identify", settings_into_dict,
					&server_general.configdata);
	}

	if (loglevel >= TRACELEVEL) {
		channel_data_defaults.debug = true;
	}

	/* reset to optind=1 to parse suricatta's argument vector */
	optind = 1;
	opterr = 0;
	while ((choice = getopt_long(argc, argv, "u:l:r:w:p:",
				     long_options, NULL)) != -1) {
		switch (choice) {
		case 'u':
			SETSTRING(server_general.url, optarg);
			mandatory_argument_count |= URL_BIT;
			break;
		case 'l':
			SETSTRING(server_general.logurl, optarg);
			break;
		case 'p':
			server_general.polling_interval =
			    (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'r':
			channel_data_defaults.retries =
			    (unsigned char)strtoul(optarg, NULL, 10);
			break;
		case 'w':
			channel_data_defaults.retry_sleep =
			    (unsigned int)strtoul(optarg, NULL, 10);
			break;
		/* Ignore not recognized options, they can be already parsed by the caller */
		case '?':
		default:
			break;
		}
	}

	if (mandatory_argument_count != ALL_MANDATORY_SET) {
		ERROR("Mandatory arguments missing!");
		suricatta_print_help();
		return SERVER_EINIT;
	}

	if (channel_curl_init() != CHANNEL_OK)
		return SERVER_EINIT;

	/*
	 * Allocate a channel to communicate with the server
	 */
	server_general.channel = channel_new();
	if (!server_general.channel)
		return SERVER_EINIT;

	if (server_general.channel->open(server_general.channel, &channel_data_defaults) != CHANNEL_OK) {
		return SERVER_EINIT;
	}

	progdata.fname = (fname) ? strdup(fname) : NULL;
	progdata.url = server_general.logurl;
	progdata.identify = &server_general.configdata;

	start_thread(server_progress_thread, &progdata);

	TRACE("General Server started !!");

	return SERVER_OK;
}

server_op_res_t server_stop(void)
{
	(void)server_general.channel->close(server_general.channel);
	return SERVER_OK;
}

server_op_res_t server_ipc(ipc_message __attribute__ ((__unused__)) *msg)
{
	return SERVER_OK;
}
