/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <assert.h>
#include <errno.h>
#include <json-c/json.h>
#include <generated/autoconf.h>
#include <util.h>
#include <network_ipc.h>
#include <sys/time.h>
#include <swupdate_status.h>
#include <pthread.h>
#include "suricatta/suricatta.h"
#include "suricatta/server.h"
#include "server_utils.h"
#include "parselib.h"
#include "channel.h"
#include "channel_curl.h"
#include "state.h"
#include "server_hawkbit.h"
#include "parselib.h"
#include "swupdate_settings.h"
#include "swupdate_dict.h"
#include "swupdate_vars.h"

#define INITIAL_STATUS_REPORT_WAIT_DELAY 10

#define JSON_OBJECT_FREED 1
#define SERVER_NAME "hawkbit"

static struct option long_options[] = {
    {"tenant", required_argument, NULL, 't'},
    {"id", required_argument, NULL, 'i'},
    {"confirm", required_argument, NULL, 'c'},
    {"url", required_argument, NULL, 'u'},
    {"polldelay", required_argument, NULL, 'p'},
    {"nocheckcert", no_argument, NULL, 'x'},
    {"retry", required_argument, NULL, 'r'},
    {"retrywait", required_argument, NULL, 'w'},
    {"proxy", optional_argument, NULL, 'y'},
    {"targettoken", required_argument, NULL, 'k'},
    {"gatewaytoken", required_argument, NULL, 'g'},
    {"interface", required_argument, NULL, 'f'},
    {"disable-token-for-dwl", no_argument, NULL, '1'},
    {"cache", required_argument, NULL, '2'},
    {"initial-report-resend-period", required_argument, NULL, 'm'},
    {"server", required_argument, NULL, 'S'},
	{"connection-timeout", required_argument, NULL, 's'},
	{"custom-http-header", required_argument, NULL, 'a'},
	{"identify", required_argument, NULL, '3'},
	{"max-download-speed", required_argument, NULL, 'l'},
    {NULL, 0, NULL, 0}};

static unsigned short mandatory_argument_count = 0;
static pthread_mutex_t notifylock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ipc_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * See hawkBit's API for an explanation
 *
 */
static const char *execution_values[] = { "closed", "proceeding", "canceled","scheduled", "rejected", "resumed", NULL };
static const char *finished_values[] = { "success", "failure", "none", NULL};

typedef struct {
	const char *key;
	const char **values;
} hawkbit_enums_t;

static hawkbit_enums_t hawkbit_enums[] = {
	{ "execution", execution_values },
	{ "finished", finished_values },
	{ NULL, NULL }, /* marker */
};

/*
 * These are used to check if all mandatory fields
 * are set
 */
#define TENANT_BIT	1
#define ID_BIT		2
#define URL_BIT		4
#define ALL_MANDATORY_SET	(TENANT_BIT | ID_BIT | URL_BIT)


extern channel_op_res_t channel_curl_init(void);
/* Prototypes for "internal" functions */
/* Note that the non-`static` functions are called from unit tests. */
static server_op_res_t server_handle_initial_state(update_state_t stateovrrd);
static int server_update_status_callback(ipc_message *msg);
server_op_res_t server_process_update_artifact(int action_id,
						json_object *json_data_artifact,
						const char *update_action,
						const char *part,
						const char *version,
						const char *name);
server_op_res_t server_set_polling_interval_json(json_object *json_root);
static server_op_res_t server_set_config_data(json_object *json_root);
server_op_res_t
server_send_deployment_reply(channel_t *channel,
			     const int action_id, const int job_cnt_max,
			     const int job_cnt_cur, const char *finished,
			     const char *execution_status, int numdetails, const char *details[]);
server_op_res_t server_send_cancel_reply(channel_t *channel, const int action_id);
static int get_target_data_length(bool locked);

server_hawkbit_t server_hawkbit = {.url = NULL,
				   .polling_interval = CHANNEL_DEFAULT_POLLING_INTERVAL,
				   .polling_interval_from_server = true,
				   .debug = false,
				   .device_id = NULL,
				   .tenant = NULL,
				   .cancel_url = NULL,
				   .update_action = NULL,
				   .usetokentodwl = true,
				   .cached_file = NULL,
				   .channel = NULL};

static channel_data_t channel_data_defaults = {.debug = false,
					       .source = SOURCE_SURICATTA,
					       .retries = CHANNEL_DEFAULT_RESUME_TRIES,
					       .retry_sleep =
						   CHANNEL_DEFAULT_RESUME_DELAY,
#ifdef CONFIG_SURICATTA_SSL
					       .usessl = true,
#endif
					       .format = CHANNEL_PARSE_JSON,
					       .nocheckanswer = false,
					       .nofollow = false,
					       .strictssl = true,
					       .max_download_speed = 0, // No download speed limit is default.
					       .noipc = false,
					       .range = NULL,
					       .connection_timeout = 0,
					       .headers = NULL,
					       .headers_to_send = NULL,
					       .received_headers = NULL
						};

static struct timeval server_time;

/*
 * Just called once to setup the tokens
 */
static inline void server_hawkbit_settoken(const char *type, const char *token)
{
	char *tokens_header = NULL;
	if (!token)
		return;

	if (ENOMEM_ASPRINTF ==
		asprintf(&tokens_header, "Authorization: %s %s", type, token)) {
			ERROR("OOM when setting %s.", type);
		return;
	}
	if (tokens_header != NULL && strlen(tokens_header))
		SETSTRING(channel_data_defaults.auth_token, tokens_header);
	free(tokens_header);
}

/*
 * This is called when a general error is found before sending the stream
 * to the installer. In this way, errors are collected in the same way as
 * when the installer is called.
 */
static inline void server_hawkbit_error(const char *s)
{
	int cnt = server_hawkbit.errorcnt;
	/* Store locally just the errors to send them back to hawkBit */
	if ((s) &&
		(cnt < HAWKBIT_MAX_REPORTED_ERRORS)) {
		server_hawkbit.errors[cnt] = strdup(s);
		if (server_hawkbit.errors[cnt])
			server_hawkbit.errorcnt++;
	}
	/*
	 * Be careful: this function should *not* be called when update
	 * is started, because the same error is sent twice
	 */
	ERROR("%s", s);
}

static bool hawkbit_enum_check(const char *key, const char *value)
{
	hawkbit_enums_t *table = hawkbit_enums;
	const char **values;

	while (table->key) {
		if (!strcmp(key, table->key)) {
			values = table->values;
			while (*values != NULL) {
				if (!strcmp(value, *values))
					return true;
				values++;
			}

			return false;
		}
		table++;
	}
	return false;
}

static const char *json_get_deployment_update_action(json_object *json_reply)
{
	if (!json_reply)
		return NULL;

	json_object *json_deployment_update_action =
	    json_get_path_key(json_reply,
			      (const char *[]){"deployment", "update", NULL});
	assert(json_object_get_type(json_deployment_update_action) ==
	       json_type_string);
	if (!json_object_get_string(json_deployment_update_action)) {
		ERROR("Server delivered empty 'update', skipping..");
		return deployment_update_action.skip;
	}

	if (strncmp(json_object_get_string(json_deployment_update_action),
		    deployment_update_action.forced,
		    strlen(deployment_update_action.forced)) == 0) {
		return deployment_update_action.forced;
	}
	if (strncmp(json_object_get_string(json_deployment_update_action),
	       	    deployment_update_action.attempt,
	       	    strlen(deployment_update_action.attempt)) == 0) {
		return deployment_update_action.attempt;
	}
	if (strncmp(json_object_get_string(json_deployment_update_action),
		    deployment_update_action.skip,
		    strlen(deployment_update_action.skip)) == 0) {
		return deployment_update_action.skip;
	}
	TRACE("Server delivered unknown 'update' field, skipping..");
	/*
	 * hawkBit API has just skip, forced, attempt, this
	 * does not happen
	 */
	return deployment_update_action.skip;
}

static void check_action_changed(int action_id, const char *update_action)
{

	if (!update_action)
		return;

	if (update_action != server_hawkbit.update_action) {
		char *notifybuf = NULL;
		server_hawkbit.update_action = update_action;
		INFO("Update classified as '%s' by server.",
			server_hawkbit.update_action);

		if (ENOMEM_ASPRINTF ==
		    asprintf(&notifybuf, "{ \"id\" : \"%d\", \"update\" : \"%s\"}",
				action_id, server_hawkbit.update_action)) {
			notify(SUBPROCESS, CHANGE, DEBUGLEVEL, "Update type changed by server");
		}  else {
			notify(SUBPROCESS, CHANGE, DEBUGLEVEL, notifybuf);
			free(notifybuf);
		}
	}
}

server_op_res_t server_send_cancel_reply(channel_t *channel, const int action_id)
{
	assert(server_hawkbit.url != NULL);
	assert(server_hawkbit.tenant != NULL);
	assert(server_hawkbit.device_id != NULL);

	/* First retry cancel URL to get stopId */
	channel_data_t channel_data_reply = channel_data_defaults;
	int stop_id = server_hawkbit.stop_id;

	/* clang-format off */
	static const char* const json_hawkbit_cancelation_feedback = STRINGIFY(
	{
		"id": %d,
		"time": "%s",
		"status": {
			"result": {
				"finished": "%s"
			},
			"execution": "%s",
			"details" : [ "%s" ]
		}
	}
	);
	/* clang-format on */
	server_op_res_t result = SERVER_OK;
	char *url = NULL;
	char *json_reply_string = NULL;

	if (ENOMEM_ASPRINTF ==
	    asprintf(&url, "%s/feedback",
		     server_hawkbit.cancel_url)) {
		ERROR("hawkBit server reply cannot be sent because of OOM.");
		result = SERVER_EINIT;
		goto cleanup;
	}

	char fdate[15 + 1];
	time_t now = time(NULL) == (time_t)-1 ? 0 : time(NULL);
	(void)strftime(fdate, sizeof(fdate), "%Y%m%dT%H%M%S", localtime(&now));
	if (ENOMEM_ASPRINTF ==
	    asprintf(&json_reply_string, json_hawkbit_cancelation_feedback,
		     stop_id, fdate, reply_status_result_finished.success,
		     reply_status_execution.closed,
		     "cancellation acknowledged.")) {
		ERROR("hawkBit server reply cannot be sent because of OOM.");
		result = SERVER_EINIT;
		goto cleanup;
	}
	channel_data_reply.url = url;
	channel_data_reply.request_body = json_reply_string;
	channel_data_reply.method = CHANNEL_POST;
	channel_data_reply.format = CHANNEL_PARSE_NONE;
	result = map_channel_retcode(channel->put(channel, (void *)&channel_data_reply));

cleanup:
	if (url != NULL) {
		free(url);
	}
	if (json_reply_string != NULL) {
		free(json_reply_string);
	}

	/*
	 * Send always a notification
	 */
	char *notifybuf = NULL;
	if (ENOMEM_ASPRINTF ==
	    asprintf(&notifybuf, "{ \"id\" : \"%d\", \"stopId\" : \"%d\"}",
		     action_id, stop_id)) {
		notify(SUBPROCESS, CANCELUPDATE, INFOLEVEL, "Update cancelled");
	}  else {
		notify(SUBPROCESS, CANCELUPDATE, INFOLEVEL, notifybuf);
		free(notifybuf);
	}

	return result;
}

static char *server_create_details(int numdetails, const char *details[])
{
	int i, ret;
	char *prev = NULL;
	char *next = NULL;

	/*
	 * Note: NEVER call TRACE / ERROR inside this function
	 * because it generates a recursion
	 */
	for (i = 0; i < numdetails; i++) {
		if (i == 0) {
			ret = asprintf(&next, "\"%s\"", details[i]);
		} else {
			ret = asprintf(&next, "%s,\"%s\"", prev, details[i]);
			free(prev);
		}
		if (ret == ENOMEM_ASPRINTF)
			return NULL;
		prev = next;
	}

	return next;
}

server_op_res_t
server_send_deployment_reply(channel_t *channel,
			     const int action_id, const int job_cnt_max,
			     const int job_cnt_cur, const char *finished,
			     const char *execution_status, int numdetails, const char *details[])
{
	assert(channel != NULL);
	assert(finished != NULL);
	assert(execution_status != NULL);
	assert(details != NULL);
	assert(server_hawkbit.url != NULL);
	assert(channel != NULL);
	assert(server_hawkbit.tenant != NULL);
	assert(server_hawkbit.device_id != NULL);

	char *detail = server_create_details(numdetails, details);

	/* clang-format off */
	static const char* const json_hawkbit_deployment_feedback = STRINGIFY(
	{
		"id": %d,
		"time": "%s",
		"status": {
			"result": {
				"progress": {
					"cnt" : %d,
					"of" : %d
				},
				"finished": "%s"
			},
			"execution": "%s",
			"details" : [ %s ]
		}
	}
	);
	/* clang-format on */
	channel_data_t channel_data = channel_data_defaults;
	server_op_res_t result = SERVER_OK;
	char *url = NULL;
	char *json_reply_string = NULL;
	char fdate[15 + 1];
	time_t now = time(NULL) == (time_t)-1 ? 0 : time(NULL);
	(void)strftime(fdate, sizeof(fdate), "%Y%m%dT%H%M%S", localtime(&now));
	if (ENOMEM_ASPRINTF ==
	    asprintf(&json_reply_string, json_hawkbit_deployment_feedback,
		     action_id, fdate, job_cnt_cur, job_cnt_max, finished,
		     execution_status, detail ? detail : " ")) {
		ERROR("hawkBit server reply cannot be sent because of OOM.");
		result = SERVER_EINIT;
		goto cleanup;
	}
	if (ENOMEM_ASPRINTF ==
	    asprintf(&url, "%s/%s/controller/v1/%s/deploymentBase/%d/feedback",
		     server_hawkbit.url, server_hawkbit.tenant,
		     server_hawkbit.device_id, action_id)) {
		ERROR("hawkBit server reply cannot be sent because of OOM.");
		result = SERVER_EINIT;
		goto cleanup;
	}
	channel_data.url = url;
	channel_data.request_body = json_reply_string;
	channel_data.format = CHANNEL_PARSE_NONE;
	channel_data.method = CHANNEL_POST;
	result = map_channel_retcode(channel->put(channel, (void *)&channel_data));

cleanup:
	if (detail != NULL)
		free(detail);
	if (url != NULL) {
		free(url);
	}
	if (json_reply_string != NULL) {
		free(json_reply_string);
	}
	return result;
}

server_op_res_t server_set_polling_interval_json(json_object *json_root)
{
	/*
	 * if poll time is ruled locally, do not read
	 * the answer from server
	 */
	if (!server_hawkbit.polling_interval_from_server)
		return SERVER_OK;

	json_object *json_data = json_get_path_key(
	    json_root, (const char *[]){"config", "polling", "sleep", NULL});
	if (json_data == NULL) {
		ERROR("Got malformed JSON: Could not find field "
		      "config->polling->sleep.");
		DEBUG("Got JSON: %s", json_object_to_json_string(json_data));
		return SERVER_EBADMSG;
	}
	static struct tm timedate;
	if (strptime(json_object_get_string(json_data), "%H:%M:%S",
		     &timedate) == NULL) {
		ERROR("Got malformed JSON: Could not convert field "
		      "config->polling->sleep to int.");
		DEBUG("Got JSON: %s", json_object_to_json_string(json_data));
		return SERVER_EBADMSG;
	}
	unsigned int polling_interval =
	    (unsigned int)(abs(timedate.tm_sec) + (abs(timedate.tm_min) * 60) +
			   (abs(timedate.tm_hour) * 60 * 60));

	/*
	 * Generally, the server sets the poll interval
	 * However, at the startup, it makes sense to speed up the process
	 * If SWUpdate is in wait state, try faster.
	 */
	if (server_hawkbit.update_state == STATE_WAIT)
		polling_interval /= 10;

	server_hawkbit.polling_interval =
	    polling_interval == 0 ? CHANNEL_DEFAULT_POLLING_INTERVAL : polling_interval;
	DEBUG("Set polling interval to %ds as announced by server.",
	      server_hawkbit.polling_interval);
	return SERVER_OK;
}

static unsigned int server_get_polling_interval(void)
{
	return server_hawkbit.polling_interval;
}

static void server_get_current_time(struct timeval *tv)
{
	struct timespec ts;

	/* use timespec for monotonic clock */
	clock_gettime(CLOCK_MONOTONIC, &ts);

	TIMESPEC_TO_TIMEVAL(tv, &ts);
}

server_op_res_t server_set_config_data(json_object *json_root)
{
	char *tmp;

	tmp = json_get_data_url(json_root, "configData");

	if (tmp != NULL) {
		pthread_mutex_lock(&ipc_lock);
		if (server_hawkbit.configData_url)
			free(server_hawkbit.configData_url);
		server_hawkbit.configData_url = tmp;
		server_hawkbit.has_to_send_configData = (get_target_data_length(true) > 0) ? true : false;
		TRACE("ConfigData: %s", server_hawkbit.configData_url);
		pthread_mutex_unlock(&ipc_lock);
	}
	return SERVER_OK;
}

static void report_server_status(int server_status)
{
	pthread_mutex_lock(&ipc_lock);
	server_hawkbit.server_status = server_status;
	server_hawkbit.server_status_time = time(NULL);
	pthread_mutex_unlock(&ipc_lock);
}

static server_op_res_t server_get_device_info(channel_t *channel, channel_data_t *channel_data)
{
	assert(channel != NULL);
	assert(channel_data != NULL);
	assert(server_hawkbit.url != NULL);
	assert(server_hawkbit.tenant != NULL);
	assert(server_hawkbit.device_id != NULL);
	DEBUG("Getting information for device '%s'",
	      server_hawkbit.device_id);
	server_op_res_t result = SERVER_OK;
	if (ENOMEM_ASPRINTF ==
	    asprintf(&channel_data->url, "%s/%s/controller/v1/%s",
		     server_hawkbit.url, server_hawkbit.tenant,
		     server_hawkbit.device_id)) {
		ERROR("hawkBit server cannot be queried because of OOM.");
		result = SERVER_EINIT;
		goto cleanup;
	}

	channel_op_res_t ch_response = channel->get(channel, (void *)channel_data);

	report_server_status(ch_response);
	if ((result = map_channel_retcode(ch_response)) !=
	    SERVER_OK) {
		goto cleanup;
	}
	if ((result = server_set_polling_interval_json(channel_data->json_reply)) !=
	    SERVER_OK) {
		goto cleanup;
	}

	if ((result = server_set_config_data(channel_data->json_reply)) !=
	    SERVER_OK) {
		goto cleanup;
	}

cleanup:
	if (channel_data->url != NULL) {
		free(channel_data->url);
	}
	return result;
}

static server_op_res_t server_get_deployment_info(channel_t *channel, channel_data_t *channel_data,
						  int *action_id)
{
	assert(channel != NULL);
	assert(channel_data != NULL);
	assert(action_id != NULL);
	server_op_res_t update_status = SERVER_OK;
	server_op_res_t result = SERVER_OK;
	char *url_deployment_base = NULL;
	char *url_cancel = NULL;
	channel_data_t channel_data_device_info = channel_data_defaults;
	if ((result = server_get_device_info(channel, &channel_data_device_info)) !=
	    SERVER_OK) {
		goto cleanup;
	}
	if ((url_cancel = json_get_data_url(channel_data_device_info.json_reply,
					    "cancelAction")) != NULL) {
		update_status = SERVER_UPDATE_CANCELED;
		channel_data->url = url_cancel;
		if (server_hawkbit.cancel_url)
			free(server_hawkbit.cancel_url);
		server_hawkbit.cancel_url = strdup(url_cancel);
		TRACE("Cancel action available at %s", url_cancel);
	} else if ((url_deployment_base =
			json_get_data_url(channel_data_device_info.json_reply,
					  "deploymentBase")) != NULL) {
		update_status = SERVER_UPDATE_AVAILABLE;
		channel_data->url = url_deployment_base;
		TRACE("Update action available at %s", url_deployment_base);
	} else {
		TRACE("No pending action on server.");
		result = SERVER_NO_UPDATE_AVAILABLE;
		goto cleanup;
	}
	if ((result = map_channel_retcode(channel->get(channel, (void *)channel_data))) !=
	    SERVER_OK) {
		goto cleanup;
	}
	json_object *json_data = json_get_path_key(
	    channel_data->json_reply, (const char *[]){"id", NULL});
	if (json_data == NULL) {
		ERROR("Got malformed JSON: Could not find field 'id'.");
		DEBUG("Got JSON: %s",
		      json_object_to_json_string(channel_data->json_reply));
		result = SERVER_EBADMSG;
		goto cleanup;
	}
	*action_id = json_object_get_int(json_data);

	/*
	 * Read stopId if cancelUpdate is detected
	 */
	server_hawkbit.stop_id = *action_id;
	if (update_status == SERVER_UPDATE_CANCELED) {
		json_data = json_get_path_key(
		    channel_data->json_reply, (const char *[]){"cancelAction", "stopId", NULL});
		if (json_data == NULL) {
			ERROR("Got malformed JSON: Could not find field 'stopId', reuse actionId.");
			DEBUG("Got JSON: %s",
			      json_object_to_json_string(channel_data->json_reply));
		} else {
			server_hawkbit.stop_id = json_object_get_int(json_data);
		}
	}
	TRACE("Associated Action ID for Update Action is %d", *action_id);
	result = update_status;

cleanup:
	if (channel_data_device_info.json_reply != NULL &&
	    json_object_put(channel_data_device_info.json_reply) !=
		JSON_OBJECT_FREED) {
		ERROR("JSON object should be freed but was not.");
	}
	if (url_cancel != NULL) {
		free(url_cancel);
	}
	if (url_deployment_base != NULL) {
		free(url_deployment_base);
	}
	return result;
}

static size_t server_check_during_dwl(char  __attribute__ ((__unused__)) *streamdata,
                                      size_t size,
                                      size_t nmemb,
                                      void  __attribute__ ((__unused__)) *data)
{
	struct timeval now;
	channel_data_t channel_data = channel_data_defaults;
	int action_id;
	int ret = size * nmemb;
	const char *update_action;

	server_get_current_time(&now);

	/*
	 * The download can take a very long time
	 * In the meantime, check with current polling time
	 * if something on the server was changed and a cancel
	 * was requested
	 */
	if ((now.tv_sec - server_time.tv_sec) < ((int)server_get_polling_interval()))
		return ret;

	/* Update current server time */
	server_time = now;

	/*
	 * We need a separate channel because we want to run
	 * a connection parallel to the download
	 */
	channel_t *channel = channel_new();
	if (!channel)
		return ret;

	if (channel->open(channel, &channel_data_defaults) != CHANNEL_OK) {
		/*
		 * it is not possible to check for a cancelUpdate,
		 * go on downloading
		 */
		channel->close(channel);
		free(channel);
		return ret;
	}

	/*
	 * Send a device Info to the hawkBit Server
	 */
	server_op_res_t result =
	    server_get_deployment_info(channel, &channel_data, &action_id);
	if (result == SERVER_UPDATE_CANCELED) {
		/* Mark that an update was cancelled by the server */
		server_hawkbit.cancelDuringUpdate = true;
		ret = 0;
	}
	update_action = json_get_deployment_update_action(channel_data.json_reply);

	/* if the deployment is skipped then stop downloading */
	if (update_action == deployment_update_action.skip)
		ret = 0;

	check_action_changed(action_id, update_action);

	/* Cleanup and free resources */
	if (channel_data.json_reply != NULL &&
	    json_object_put(channel_data.json_reply) != JSON_OBJECT_FREED) {
		ERROR("JSON object should be freed but was not.");
	}
	channel->close(channel);
	free(channel);

	return ret;
}

static server_op_res_t server_has_pending_action(int *action_id)
{

	channel_data_t channel_data = channel_data_defaults;
	const char *update_action = NULL;
	server_op_res_t result =
	    server_get_deployment_info(server_hawkbit.channel,
			    		&channel_data, action_id);

	/*
	 * Retrieve if "update" changed before freeing object, used later
	 */
	if (result == SERVER_UPDATE_AVAILABLE) {
		update_action = json_get_deployment_update_action(channel_data.json_reply);
	}

	/* TODO don't throw away reply JSON as it's fetched again by succinct
	 *      server_install_update() */
	if (channel_data.json_reply != NULL &&
	    json_object_put(channel_data.json_reply) != JSON_OBJECT_FREED) {
		ERROR("JSON object should be freed but was not.");
	}
	if (result == SERVER_UPDATE_CANCELED) {
		DEBUG("Acknowledging cancelled update.");
		/* Inform the installer that a CANCEL was received */
		(void)server_send_cancel_reply(server_hawkbit.channel, *action_id);

		server_hawkbit.update_state = STATE_OK;
		/*
		 * Save the state
		 */
		if ((result = save_state(STATE_OK)) != SERVER_OK) {
			ERROR("Error while resetting update state on persistent "
			"storage.\n");
		}
		return SERVER_OK;
	}

	/*
	 * First check if initialization was completed or
	 * a feedback should be sent to hawkBit
	 */
	if (server_hawkbit.update_state == STATE_WAIT) {
		return SERVER_OK;
	}

	/*
	 * if configData was not yet sent,
	 * send it without asking for deviceInfo
	 */
	if (server_hawkbit.has_to_send_configData)
		return SERVER_ID_REQUESTED;


	if ((result == SERVER_UPDATE_AVAILABLE) &&
	    (get_state() == STATE_INSTALLED)) {
		WARN("An already installed update is pending testing, "
		     "ignoring available update action.");
		INFO("Please restart SWUpdate to report the test results "
		     "upstream.");
		check_action_changed(*action_id, update_action);
		result = SERVER_NO_UPDATE_AVAILABLE;
	}

	return result;
}

static void add_detail_error(const char *s)
{
	int cnt = server_hawkbit.errorcnt;
	/* Store locally just the errors to send them back to hawkBit */
	if ((s) && (!strncmp(s, "ERROR", 5)) &&
		(cnt < HAWKBIT_MAX_REPORTED_ERRORS)) {

		server_hawkbit.errors[cnt] = strdup(s);
		if (server_hawkbit.errors[cnt])
			server_hawkbit.errorcnt++;
	}
}

static server_op_res_t handle_feedback(int action_id, server_op_res_t result,
					update_state_t state,
					const char *reply_result,
					const char *reply_execution,
					int numdetails,
					const char *details[])
{

	switch (result) {
	case SERVER_OK:
	case SERVER_ID_REQUESTED:
	case SERVER_UPDATE_CANCELED:
	case SERVER_NO_UPDATE_AVAILABLE:
		TRACE("No active update available, nothing to report to "
		      "server.\n");
		if ((state != STATE_OK) && (state != STATE_NOT_AVAILABLE)) {
			WARN("Persistent state=%c but no active update on "
			     "server?!\n",
			     state);
		}
		return SERVER_OK;
	case SERVER_EERR:
	case SERVER_EBADMSG:
	case SERVER_EINIT:
	case SERVER_EACCES:
	case SERVER_EAGAIN:
		return result;
	case SERVER_UPDATE_AVAILABLE:
		break;
	}

	if (server_send_deployment_reply(server_hawkbit.channel, action_id, 0, 0, reply_result,
					 reply_execution,
					 numdetails, details) != SERVER_OK) {
		ERROR("Error while reporting installation status to server.");
		return SERVER_EAGAIN;
	}

	return SERVER_UPDATE_AVAILABLE;
}

static void get_action_id_from_env(int *action_id)
{
	/*
	 * Retrieve the action_id if SWUpdate stored it
	 * during the update
	 * The current action_id on the server can be different if the operator
	 * cancelled a rollout and started a new one while SWUpdate was
	 * restarting.
	 * Get the acction_id that corresponds to the done update if it was
	 * stored.
	 */
	char *action_str = swupdate_vars_get("action_id", NULL);
	if (action_str) {
		int tmp = ustrtoull(action_str, NULL, 10);
		/*
		 * action_id = 0 is invalid, then check it
		 */
		if (tmp > 0) {
			*action_id = tmp;
			TRACE("Retrieve action_id from previous run: %d", *action_id);
		}
		free(action_str);
	}
}

server_op_res_t server_handle_initial_state(update_state_t stateovrrd)
{
	int action_id;
	update_state_t state = STATE_OK;
	if (stateovrrd != STATE_NOT_AVAILABLE) {
		state = stateovrrd;
		TRACE("Got state=%c from command line.", state);
		if (!is_valid_state(state)) {
			return SERVER_EINIT;
		}
	} else {
		if ((state = get_state()) == STATE_ERROR) {
			return SERVER_EINIT;
		}
	}

	const char *reply_result;
	const char *reply_execution;
	const char *reply_message;
	switch (state) {
	case STATE_INSTALLED:
		reply_result = reply_status_result_finished.none;
		reply_execution = reply_status_execution.proceeding;
		reply_message = "Update Installed, Testing Pending.";
		break;
	case STATE_TESTING:
		reply_result = reply_status_result_finished.success;
		reply_execution = reply_status_execution.closed;
		reply_message = "Update Installed.";
		break;
	case STATE_FAILED:
		reply_result = reply_status_result_finished.failure;
		reply_execution = reply_status_execution.closed;
		reply_message = "Update Failed.";
		break;
	case STATE_OK:
	case STATE_NOT_AVAILABLE:
	default:
		DEBUG("State is STATE_OK/STATE_NOT_AVAILABLE, nothing to "
		      "report to server.\n");
		return SERVER_OK;
	}
	server_op_res_t result;

	/*
	 * Try to retrieve current action id
	 */

	channel_data_t channel_data = channel_data_defaults;
	result = server_get_deployment_info(server_hawkbit.channel,
						    &channel_data, &action_id);

	/*
	 * Get action_id from env, if any
	 */
	get_action_id_from_env(&action_id);

	result = handle_feedback(action_id, result, state, reply_result,
				 reply_execution, 1, &reply_message);

	if (result != SERVER_UPDATE_AVAILABLE)
		return result;

	/*
	 * Everything fine, reset action_id if any
	 */
	swupdate_vars_set("action_id", NULL, NULL);

	/* NOTE (Re-)setting STATE_KEY=STATE_OK == '0' instead of deleting it
	 *      as it may be required for the switchback/recovery U-Boot logics.
	 */
	if ((result = save_state(STATE_OK)) != SERVER_OK) {
		ERROR("Error while resetting update state on persistent "
		      "storage.\n");
		return result;
	}
	return SERVER_OK;
}

static int server_update_status_callback(ipc_message *msg)
{
	/* Store locally just the errors to send them back to hawkBit */
	add_detail_error(msg->data.status.desc);

	return 0;
}

static void *process_notification_thread(void *data)
{
	const int action_id = *(int *)data;
	const int MAX_DETAILS = 48;
	const char *details[MAX_DETAILS];
	unsigned int numdetails = 0;
	bool stop = false;
	channel_data_t channel_data = channel_data_defaults;
	unsigned int percent = 0;
	unsigned int step = 0;


	/*
	 * Create a new channel to the server. The opened channel is
	 * used to download the SWU
	 */
	channel_t *channel = channel_new();
	if (!channel)
		return NULL;

	if (channel->open(channel, &channel_data) != CHANNEL_OK) {
		channel->close(channel);
		free(channel);
		return NULL;
	}

	for (;;) {
		ipc_message msg;
		bool data_avail = false;
		int ret = ipc_get_status_timeout(&msg, 100);

		data_avail = ret > 0 && (strlen(msg.data.status.desc) != 0);

		/*
		 * Mutex used to synchronize end of the thread
		 * The suricatta thread locks the mutex at the beginning
		 * and release it when this task should terminate.
		 * By getting the lock, this task loads know it has to finish
		 */
		if (!pthread_mutex_trylock(&notifylock)) {
			stop = true;
		}

		if (data_avail && msg.data.status.current == PROGRESS)
			continue;
		/*
		 * If there is a message
		 * ret > 0: data available
		 * ret == 0: TIMEOUT, no more messages
		 * ret < 0 : ERROR, exit
		 */
		if (data_avail && numdetails < MAX_DETAILS) {
			for (int c = 0; c < strlen(msg.data.status.desc); c++) {
				switch (msg.data.status.desc[c]) {
				case '"':
				case '\'':
				case '\\':
				case '\n':
				case '\r':
					msg.data.status.desc[c] = ' ';
					break;
				}
			}
			details[numdetails++] = strdup(msg.data.status.desc);
		}

		/*
		 * Flush to the server
		 */
		if (numdetails == MAX_DETAILS || (stop && !data_avail)) {
			TRACE("Update log to server from thread");
			if (server_send_deployment_reply(
				channel,
				action_id, step, percent,
				reply_status_result_finished.none,
				reply_status_execution.proceeding, numdetails,
				&details[0]) != SERVER_OK) {
				ERROR("Error while sending log to server.");
			}
			for (unsigned int count = 0; count < numdetails; count++) {
				free((void *)details[count]);
			}
			numdetails = 0;
			percent++;
			if (percent > 100) {
				percent = 0;
				step++;
			}
		}

		if (stop && !data_avail)
			break;
	}

	pthread_mutex_unlock(&notifylock);

	/*
	 * Now close the channel for feedback
	 */
	channel->close(channel);
	free(channel);

	return NULL;
}

server_op_res_t server_process_update_artifact(int action_id,
						json_object *json_data_artifact,
						const char *update_action,
						const char *part,
						const char *version,
						const char *name)
{
	channel_t *channel = server_hawkbit.channel;
	assert(channel != NULL);
	assert(json_data_artifact != NULL);
	assert(json_object_get_type(json_data_artifact) == json_type_array);
	server_op_res_t result = SERVER_OK;
	pthread_t notify_to_hawkbit_thread;

	/* Initialize list of errors */
	for (int i = 0; i < HAWKBIT_MAX_REPORTED_ERRORS; i++)
		server_hawkbit.errors[i] = NULL;
	server_hawkbit.errorcnt = 0;

	struct array_list *json_data_artifact_array =
	    json_object_get_array(json_data_artifact);
	int json_data_artifact_max =
	    json_object_array_length(json_data_artifact);
	int json_data_artifact_installed = 0;
	json_object *json_data_artifact_item = NULL;

	char *action_id_str;
	if (asprintf(&action_id_str, "%d", action_id) == ENOMEM_ASPRINTF) {
		ERROR("OOM reached when saving action_id");
		return SERVER_EERR;
	}
	if (swupdate_vars_set("action_id", action_id_str, NULL)) {
		WARN("Action_id cannot be stored, do yourself");
	}
	free(action_id_str);

	for (int json_data_artifact_count = 0;
	     json_data_artifact_count < json_data_artifact_max;
	     json_data_artifact_count++) {
		int thread_ret = -1;
		json_data_artifact_item = array_list_get_idx(
		    json_data_artifact_array, json_data_artifact_count);
		TRACE("Iterating over JSON, key=%s",
		      json_object_to_json_string(json_data_artifact_item));
		json_object *json_data_artifact_filename =
		    json_get_path_key(json_data_artifact_item,
				      (const char *[]){"filename", NULL});
		json_object *json_data_artifact_sha1hash =
		    json_get_path_key(json_data_artifact_item,
				      (const char *[]){"hashes", "sha1", NULL});
		json_object *json_data_artifact_size = json_get_path_key(
		    json_data_artifact_item, (const char *[]){"size", NULL});

		/* hawkBit reports either two URLs, one URL, or none at all
		 * depending on whether hawkBit is configured for HTTPS, HTTP,
		 * or none, respectively. */
		json_object *json_data_artifact_url_https = json_get_path_key(
		    json_data_artifact_item,
		    (const char *[]){"_links", "download", "href", NULL});
		json_object *json_data_artifact_url_http = json_get_path_key(
		    json_data_artifact_item,
		    (const char *[]){"_links", "download-http", "href", NULL});
#ifndef CONFIG_SURICATTA_SSL
		if (json_data_artifact_url_http == NULL) {
			server_hawkbit_error("No artifact download HTTP URL reported by "
			      "server.");
			result = SERVER_EBADMSG;
			goto cleanup;
		}
#endif
		/* TODO fall-back to HTTP in case HTTPS isn't available? */
		json_object *json_data_artifact_url =
		    json_data_artifact_url_https == NULL
			? json_data_artifact_url_http
			: json_data_artifact_url_https;
		if (json_data_artifact_url == NULL) {
			server_hawkbit_error("No artifact download URL reported by server.\n");
			result = SERVER_EBADMSG;
			goto cleanup;
		}

		if (json_data_artifact_filename == NULL ||
		    json_data_artifact_sha1hash == NULL ||
		    json_data_artifact_size == NULL) {
			server_hawkbit_error(
			    "Got malformed JSON: Could not find fields "
			    "'filename', 'hashes->sha1', or 'size' in JSON.\n");
			DEBUG("Got JSON: %s", json_object_to_json_string(
						    json_data_artifact_item));
			result = SERVER_EBADMSG;
			goto cleanup;
		}
		assert(json_object_get_type(json_data_artifact_filename) ==
		       json_type_string);
		assert(json_object_get_type(json_data_artifact_sha1hash) ==
		       json_type_string);
		assert(json_object_get_type(json_data_artifact_size) ==
		       json_type_int);
		assert(json_object_get_type(json_data_artifact_url) ==
		       json_type_string);

		/*
		 * Check if the file is a .swu image
		 * and skip if it not
		 */
		const char *s = json_object_get_string(json_data_artifact_filename);
		int endfilename = strlen(s) - strlen(".swu");
		if (endfilename <= 0 ||
		    strncmp(&s[endfilename], ".swu", 4)) {
			DEBUG("File '%s' is not a SWU image, skipping", s);
			continue;
		}

		DEBUG("Processing '%s' from '%s'",
		      json_object_get_string(json_data_artifact_filename),
		      json_object_get_string(json_data_artifact_url));

		channel_data_t channel_data = channel_data_defaults;
		channel_data.url =
		    strdup(json_object_get_string(json_data_artifact_url));

		static const char* const update_info = STRINGIFY(
		{
		"server": "%s",
		"update": "%s",
		"part": "%s",
		"version": "%s",
		"name": "%s",
		"id" : "%d"
		}
		);
		if (ENOMEM_ASPRINTF ==
		    asprintf(&channel_data.info, update_info,
			    SERVER_NAME,
			    update_action,
			    part,
			    version,
			    name, action_id)) {
			ERROR("hawkBit server reply cannot be sent because of OOM.");
			result = SERVER_EBADMSG;
			goto cleanup_loop;
		}

		channel_data.dwlwrdata = server_check_during_dwl;

		/*
		 * There is no authorizytion token when file is loaded, because SWU
		 * can be on a different server as hawkBit with a different
		 * authorization method.
		 */
		if (!server_hawkbit.usetokentodwl)
			channel_data.auth_token = NULL;

		/*
		 * If there is a cached file, try to read the SWU from the file
		 * first and then load the remaining from network
		 */
		if (server_hawkbit.cached_file)
			channel_data.cached_file = server_hawkbit.cached_file;

		/*
		 * Retrieve current time to check download time
		 * This is used in the callback to ask again the hawkBit
		 * server if the download is longer as the polling time
		 */

		server_get_current_time(&server_time);

		/*
		 * Start background task to collect logs and
		 * send to hawkBit server
		 */
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

		pthread_mutex_init(&notifylock, NULL);
		pthread_mutex_lock(&notifylock);

		thread_ret = pthread_create(&notify_to_hawkbit_thread, &attr,
				process_notification_thread, &action_id);

		channel_op_res_t cresult =
		    channel->get_file(channel, (void *)&channel_data);
		if ((result = map_channel_retcode(cresult)) != SERVER_OK) {
			/* this is called to collect errors */
			ipc_wait_for_complete(server_update_status_callback);
			goto cleanup_loop;
		}

#ifdef CONFIG_SURICATTA_SSL
		if (strncmp((char *)&channel_data.sha1hash,
			    json_object_get_string(json_data_artifact_sha1hash),
			    SWUPDATE_SHA_DIGEST_LENGTH) != 0) {
				ERROR(
			    "Checksum does not match: Should be '%s', but "
			    "actually is '%s'.\n",
			    json_object_get_string(json_data_artifact_sha1hash),
			    channel_data.sha1hash);
			ipc_wait_for_complete(server_update_status_callback);
			result = SERVER_EBADMSG;
			goto cleanup_loop;
		}
		DEBUG("Downloaded artifact's checksum matches server's: "
		      "'%s'.\n",
		      channel_data.sha1hash);
#endif

		switch (ipc_wait_for_complete(server_update_status_callback)) {
		case DOWNLOAD:
		case IDLE:
		case START:
		case RUN:
		case SUCCESS:
			result = SERVER_OK;
			json_data_artifact_installed++;
			break;
		case FAILURE:
			result = SERVER_EERR;
			goto cleanup_loop;
		}

	cleanup_loop:
		pthread_mutex_unlock(&notifylock);
		if (!thread_ret) {
			if (pthread_join(notify_to_hawkbit_thread, NULL)) {
				ERROR("return code from pthread_join()");
			}
		}
		pthread_mutex_destroy(&notifylock);
		if (channel_data.url != NULL) {
			free(channel_data.url);
		}
		if (channel_data.info != NULL) {
			free(channel_data.info);
		}
		if (result != SERVER_OK) {
			break;
		}
	}
cleanup:
	/* Nothing installed ? Report that something was wrong */
	if (!json_data_artifact_installed) {
		server_hawkbit_error("No suitable .swu image found");
		result = SERVER_EERR;
	}
	if (server_hawkbit.cached_file) {
		free(server_hawkbit.cached_file);
		server_hawkbit.cached_file = NULL;
	}

	return result;
}

static server_op_res_t server_install_update(void)
{
	int action_id;
	channel_data_t channel_data = channel_data_defaults;
	server_op_res_t result =
	    server_get_deployment_info(server_hawkbit.channel, &channel_data, &action_id);
	switch (result) {
	case SERVER_UPDATE_CANCELED:
	case SERVER_UPDATE_AVAILABLE:
	case SERVER_ID_REQUESTED:
	case SERVER_OK:
		break;
	case SERVER_EERR:
	case SERVER_EBADMSG:
	case SERVER_EINIT:
	case SERVER_EACCES:
	case SERVER_EAGAIN:
	case SERVER_NO_UPDATE_AVAILABLE:
		goto cleanup;
	}

	server_hawkbit.update_action = NULL;
	const char *update_action = json_get_deployment_update_action(channel_data.json_reply);
	check_action_changed(action_id, update_action);
	if (server_hawkbit.update_action == deployment_update_action.skip) {
		const char *details = "Skipped Update.";
		if (server_send_deployment_reply(
			server_hawkbit.channel,
			action_id, 0, 0, reply_status_result_finished.success,
			reply_status_execution.closed, 1,
			&details) != SERVER_OK) {
			ERROR("Error while reporting installation progress to "
			      "server.\n");
		}
		goto cleanup;
	}

	json_object *json_data_chunk =
	    json_get_path_key(channel_data.json_reply,
			      (const char *[]){"deployment", "chunks", NULL});
	if (json_data_chunk == NULL) {
		server_hawkbit_error("Got malformed JSON: Could not find field "
		      "deployment->chunks.");
		DEBUG("Got JSON: %s",
		      json_object_to_json_string(channel_data.json_reply));
		result = SERVER_EBADMSG;
		goto cleanup;
	}

	assert(json_object_get_type(json_data_chunk) == json_type_array);
	struct array_list *json_data_chunk_array =
	    json_object_get_array(json_data_chunk);
	int json_data_chunk_count = 0;
	int json_data_chunk_max = json_object_array_length(json_data_chunk);
	json_object *json_data_chunk_item = NULL;
	const char *details[] = {"Installing Update Chunk Artifacts.",
				 "Installing Update Chunk Artifacts failed.",
				 "Installed Chunk.",
				 "All Chunks Installed."};

	for (json_data_chunk_count = 0;
	     json_data_chunk_count < json_data_chunk_max;
	     json_data_chunk_count++) {
		json_data_chunk_item = array_list_get_idx(
		    json_data_chunk_array, json_data_chunk_count);
		TRACE("Iterating over JSON, key=%s",
		      json_object_to_json_string(json_data_chunk_item));
		json_object *json_data_chunk_part = json_get_path_key(
		    json_data_chunk_item, (const char *[]){"part", NULL});
		json_object *json_data_chunk_version = json_get_path_key(
		    json_data_chunk_item, (const char *[]){"version", NULL});
		json_object *json_data_chunk_name = json_get_path_key(
		    json_data_chunk_item, (const char *[]){"name", NULL});
		if ((json_data_chunk_part == NULL) ||
		    (json_data_chunk_version == NULL) ||
		    (json_data_chunk_name == NULL)) {
			server_hawkbit_error("Got malformed JSON: Could not find fields "
			      "'part', 'version', or 'name'.");
			DEBUG("Got JSON: %s", json_object_to_json_string(
						    channel_data.json_reply));
			result = SERVER_EBADMSG;
			goto cleanup;
		}
		assert(json_object_get_type(json_data_chunk_part) ==
		       json_type_string);
		assert(json_object_get_type(json_data_chunk_name) ==
		       json_type_string);
		assert(json_object_get_type(json_data_chunk_version) ==
		       json_type_string);
		DEBUG("Processing Update Chunk '%s', version %s, part %s",
		      json_object_get_string(json_data_chunk_name),
		      json_object_get_string(json_data_chunk_version),
		      json_object_get_string(json_data_chunk_part));

		json_object *json_data_chunk_artifacts = json_get_path_key(
		    json_data_chunk_item, (const char *[]){"artifacts", NULL});
		if (json_data_chunk_artifacts == NULL) {
			server_hawkbit_error("Got malformed JSON: Could not find field "
			      "deployment->chunks->artifacts.");
			DEBUG("Got JSON: %s", json_object_to_json_string(
						    channel_data.json_reply));
			result = SERVER_EBADMSG;
			goto cleanup;
		}

		if (server_send_deployment_reply(
			server_hawkbit.channel,
			action_id, json_data_chunk_max, json_data_chunk_count,
			reply_status_result_finished.none,
			reply_status_execution.proceeding, 1,
			&details[0]) != SERVER_OK) {
			ERROR("Error while reporting installation progress to "
			      "server.\n");
			goto cleanup;
		}
		assert(json_object_get_type(json_data_chunk_artifacts) ==
		       json_type_array);
		/* reset flag, will be set if a cancel is detected */
		server_hawkbit.cancelDuringUpdate = false;
		result =
		    server_process_update_artifact(action_id, json_data_chunk_artifacts,
				server_hawkbit.update_action,
				json_object_get_string(json_data_chunk_part),
				json_object_get_string(json_data_chunk_version),
				json_object_get_string(json_data_chunk_name));

		if (result != SERVER_OK) {

			/* Check if failed because it was cancelled */
			if (server_hawkbit.cancelDuringUpdate) {
				TRACE("Acknowledging cancelled update.");
				(void)server_send_cancel_reply(server_hawkbit.channel, action_id);
				/* Inform the installer that a CANCEL was received */
			} else {
				/* TODO handle partial installations and rollback if
				 *      more than one artifact is available on hawkBit.
				 */
				ERROR("Error processing update chunk named '%s', "
				      "version %s, part %s\n",
				json_object_get_string(json_data_chunk_name),
			        json_object_get_string(json_data_chunk_version),
			        json_object_get_string(json_data_chunk_part));
				(void)server_send_deployment_reply(
				    server_hawkbit.channel,
			 	    action_id, json_data_chunk_max,
				    json_data_chunk_count,
				    reply_status_result_finished.failure,
				    reply_status_execution.closed, server_hawkbit.errorcnt,
				    (const char **)server_hawkbit.errors);
			}
			goto cleanup;
		}
		if (server_send_deployment_reply(
			server_hawkbit.channel,
			action_id, json_data_chunk_max,
			json_data_chunk_count + 1,
			reply_status_result_finished.none,
			reply_status_execution.proceeding, 1,
			&details[2]) != SERVER_OK) {
			ERROR("Error while reporting installation progress to "
			      "server.\n");
		}
	}

	if (server_send_deployment_reply(
		server_hawkbit.channel,
		action_id, json_data_chunk_max, json_data_chunk_count,
		reply_status_result_finished.none,
		reply_status_execution.proceeding, 1,
		&details[3]) != SERVER_OK) {
		ERROR("Error while reporting installation success to "
		      "server.\n");
	}

cleanup:
	for (int i = 0; i < HAWKBIT_MAX_REPORTED_ERRORS; i++) {
		if (server_hawkbit.errors[i]) {
			free(server_hawkbit.errors[i]);
			server_hawkbit.errors[i] = NULL;
		}
	}
	if (channel_data.json_reply != NULL &&
	    json_object_put(channel_data.json_reply) != JSON_OBJECT_FREED) {
		ERROR("JSON object should be freed but was not.");
	}
	if (result == SERVER_OK) {
		INFO("Update successful, executing post-update actions.");
		ipc_message msg;
		memset(&msg, 0, sizeof(msg));
		if (ipc_postupdate(&msg) != 0) {
			result = SERVER_EERR;
		} else {
			result = msg.type == ACK ? SERVER_OK : SERVER_EERR;
			DEBUG("%s", msg.data.msg);
		}
	}
	return result;
}

int get_target_data_length(bool locked)
{
	int len = 0;
	struct dict_entry *entry;

	if (!locked)
		pthread_mutex_lock(&ipc_lock);
	LIST_FOREACH(entry, &server_hawkbit.configdata, next) {
		char *key = dict_entry_get_key(entry);
		char *value = dict_entry_get_value(entry);

		len += strlen(key) + strlen(value) + strlen (" : ") + 6;
	}
	if (!locked)
		pthread_mutex_unlock(&ipc_lock);

	return len;
}

static server_op_res_t server_send_target_data(void)
{
	channel_t *channel = server_hawkbit.channel;
	struct dict_entry *entry;
	bool first = true;
	int len = 0;
	server_op_res_t result = SERVER_OK;
	char *json_reply_string = NULL;
	char *url = NULL;

	assert(channel != NULL);
	len = get_target_data_length(false);

	if (!len) {
		server_hawkbit.has_to_send_configData = false;
		return SERVER_OK;
	}

	char *configData = (char *)(malloc(len + 16));
	if (!configData) {
		ERROR("OOM when sending ID data to server");
		return SERVER_EERR;
	}
	memset(configData, 0, len + 16);

	static const char* const config_data = STRINGIFY(
		%c"%s": "%s"
	);

	char *keyvalue = NULL;
	pthread_mutex_lock(&ipc_lock);
	LIST_FOREACH(entry, &server_hawkbit.configdata, next) {
		char *key = dict_entry_get_key(entry);
		char *value = dict_entry_get_value(entry);

		if (ENOMEM_ASPRINTF ==
		    asprintf(&keyvalue, config_data,
				((first) ? ' ' : ','),
				key,
				value)) {
			ERROR("hawkBit server reply cannot be sent because of OOM.");
			result = SERVER_EINIT;
			pthread_mutex_unlock(&ipc_lock);
			goto cleanup;
		}
		first = false;
		TRACE("KEYVALUE=%s %s %s", keyvalue, key, value);
		strcat(configData, keyvalue);
		free(keyvalue);

	}
	pthread_mutex_unlock(&ipc_lock);

	TRACE("CONFIGDATA=%s", configData);

	static const char* const json_hawkbit_config_data = STRINGIFY(
	{
		"id": "%s",
		"time": "%s",
		"mode" : "replace",
		"status": {
			"result": {
				"finished": "%s"
			},
			"execution": "%s",
			"details" : [ "%s" ]
		},
		"data" : {
			%s
		}
	}
	);

	channel_data_t channel_data_reply = channel_data_defaults;
	char fdate[15 + 1];
	time_t now = time(NULL) == (time_t)-1 ? 0 : time(NULL);
	(void)strftime(fdate, sizeof(fdate), "%Y%m%dT%H%M%S", localtime(&now));

	if (ENOMEM_ASPRINTF ==
	    asprintf(&json_reply_string, json_hawkbit_config_data,
		     "", fdate, reply_status_result_finished.success,
		     reply_status_execution.closed,
		     "", configData)) {
		ERROR("hawkBit server reply cannot be sent because of OOM.");
		result = SERVER_EINIT;
		goto cleanup;
	}
	if (ENOMEM_ASPRINTF ==
	    asprintf(&url, "%s/%s/controller/v1/%s/configData",
		     server_hawkbit.url, server_hawkbit.tenant,
		     server_hawkbit.device_id)) {
		ERROR("hawkBit server reply cannot be sent because of OOM.");
		result = SERVER_EINIT;
		goto cleanup;
	}

	channel_data_reply.url = url;
	channel_data_reply.format = CHANNEL_PARSE_NONE;
	channel_data_reply.request_body = json_reply_string;
	TRACE("URL=%s JSON=%s", channel_data_reply.url, channel_data_reply.request_body);
	channel_data_reply.method = CHANNEL_PUT;
	result = map_channel_retcode(channel->put(channel, (void *)&channel_data_reply));

	if (result == SERVER_OK)
		server_hawkbit.has_to_send_configData = false;

cleanup:

	free(configData);
	if (url != NULL)
		free(url);

	if (json_reply_string)
		free(json_reply_string);

	return result;
}

static void server_print_help(void)
{
	fprintf(
	    stdout,
	    "\t  -t, --tenant      * Set hawkBit tenant ID for this device.\n"
	    "\t  -u, --url         * Host and port of the hawkBit instance, "
	    "e.g., localhost:8080\n"
	    "\t  -i, --id          * The device ID to communicate to hawkBit.\n"
	    "\t  -c, --confirm       Confirm update status to server: 1=AGAIN, "
	    "2=SUCCESS, 3=FAILED\n"
	    "\t  -x, --nocheckcert   Do not abort on flawed server "
	    "certificates.\n"
	    "\t  -p, --polldelay     Delay in seconds between two hawkBit "
	    "poll operations (default: %ds).\n"
	    "\t  -r, --retry         Resume and retry interrupted downloads "
	    "(default: %d tries).\n"
	    "\t  -w, --retrywait     Time to wait prior to retry and "
	    "resume a download (default: %ds).\n"
	    "\t  -y, --proxy         Use proxy. Either give proxy URL, else "
	    "{http,all}_proxy env is tried.\n"
	    "\t  -k, --targettoken   Set target token.\n"
	    "\t  -g, --gatewaytoken  Set gateway token.\n"
	    "\t  -f, --interface     Set the network interface to connect to hawkBit.\n"
	    "\t  --disable-token-for-dwl Do not send authentication header when downlloading SWU.\n"
	    "\t  --cache <file>      Use cache file as starting SWU\n"
	    "\t  -m, --initial-report-resend-period <seconds> Time to wait prior to retry "
	    "sending initial state with '-c' option (default: %ds).\n"
	    "\t  -s, --connection-timeout Set the server connection timeout (default: 300s).\n"
	    "\t  -a, --custom-http-header <name> <value> Set custom HTTP header, "
	    "appended to every HTTP request being sent.\n"
	    "\t  --identify <name> <value> Set custom device attributes for Suricatta.\n"
	    "\t  -n, --max-download-speed <limit>  Set download speed limit.\n"
	    "\t                                    Example: -n 100k; -n 1M; -n 100; -n 1G\n",
	    CHANNEL_DEFAULT_POLLING_INTERVAL, CHANNEL_DEFAULT_RESUME_TRIES,
	    CHANNEL_DEFAULT_RESUME_DELAY,
	    INITIAL_STATUS_REPORT_WAIT_DELAY);
}

static int server_hawkbit_settings(void *elem, void  __attribute__ ((__unused__)) *data)
{
	char tmp[128];

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "tenant", tmp);
	if (strlen(tmp)) {
		SETSTRING(server_hawkbit.tenant, tmp);
		mandatory_argument_count |= TENANT_BIT;
	}
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "id", tmp);
	if (strlen(tmp)) {
		SETSTRING(server_hawkbit.device_id, tmp);
		mandatory_argument_count |= ID_BIT;
	}
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "url", tmp);
	if (strlen(tmp)) {
		SETSTRING(server_hawkbit.url, tmp);
		mandatory_argument_count |= URL_BIT;
	}

	get_field(LIBCFG_PARSER, elem, "polldelay",
		&server_hawkbit.polling_interval);

	get_field(LIBCFG_PARSER, elem, "initial-report-resend-period",
		&server_hawkbit.initial_report_resend_period);

	channel_settings(elem, &channel_data_defaults);

	get_field(LIBCFG_PARSER, elem, "usetokentodwl",
		&server_hawkbit.usetokentodwl);

	get_field(LIBCFG_PARSER, elem, "connection-timeout",
		&channel_data_defaults.connection_timeout);

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "targettoken", tmp);
	if (strlen(tmp))
		SETSTRING(server_hawkbit.targettoken, tmp);
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "gatewaytoken", tmp);
	if (strlen(tmp))
		SETSTRING(server_hawkbit.gatewaytoken, tmp);

	return 0;

}

static server_op_res_t server_start(const char *fname, int argc, char *argv[])
{
	update_state_t update_state = STATE_NOT_AVAILABLE;
	int choice = 0;

	mandatory_argument_count = 0;

	pthread_mutex_lock(&ipc_lock);
	LIST_INIT(&server_hawkbit.configdata);
	LIST_INIT(&server_hawkbit.httpheaders);

	server_hawkbit.initial_report_resend_period = INITIAL_STATUS_REPORT_WAIT_DELAY;
	if (fname) {
		swupdate_cfg_handle handle;
		swupdate_cfg_init(&handle);
		if (swupdate_cfg_read_file(&handle, fname) == 0) {
			/*
			 * Search "suricatta" section to be compatible with past
			 */
			read_module_settings(&handle, "suricatta", server_hawkbit_settings, NULL);
			/*
			 * Then try "hawkBit" because each server has its own
			 * section
			 */
			read_module_settings(&handle, "hawkbit", server_hawkbit_settings, NULL);
			read_module_settings(&handle, "identify", settings_into_dict, &server_hawkbit.configdata);

			read_module_settings(&handle, "custom-http-headers",
					settings_into_dict, &server_hawkbit.httpheaders);
		}
		swupdate_cfg_destroy(&handle);
	}
	pthread_mutex_unlock(&ipc_lock);

	if (loglevel >= DEBUGLEVEL) {
		server_hawkbit.debug = true;
		channel_data_defaults.debug = true;
	}

	/* reset to optind=1 to parse suricatta's argument vector */
	optind = 1;
	opterr = 0;
	while ((choice = getopt_long(argc, argv, "t:i:c:u:p:xr:y::w:k:g:f:2:m:s:a:n:S:",
				     long_options, NULL)) != -1) {
		switch (choice) {
		case 't':
			SETSTRING(server_hawkbit.tenant, optarg);
			mandatory_argument_count |= TENANT_BIT;
			break;
		case 'i':
			SETSTRING(server_hawkbit.device_id, optarg);
			mandatory_argument_count |= ID_BIT;
			break;
		case 'k':
			SETSTRING(server_hawkbit.targettoken, optarg);
			break;
		case 'g':
			SETSTRING(server_hawkbit.gatewaytoken, optarg);
			break;
		case 'c':
			/* When no persistent update state storage is available,
			 * use command line switch to instruct what to report.
			 */
			update_state = (unsigned int)*optarg;
			switch (update_state) {
			case STATE_OK:
			case STATE_INSTALLED:
			case STATE_TESTING:
			case STATE_FAILED:
			case STATE_WAIT:
				break;
			default:
				fprintf(
				    stderr,
				    "Error: Invalid update status given.\n");
				suricatta_print_help();
				exit(EXIT_FAILURE);
			}
			break;
		case 'u':
			SETSTRING(server_hawkbit.url, optarg);
			mandatory_argument_count |= URL_BIT;
			break;
		case 'p':
			server_hawkbit.polling_interval =
			    (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'x':
			channel_data_defaults.strictssl = false;
			break;
		case 'r':
			channel_data_defaults.retries =
			    (unsigned char)strtoul(optarg, NULL, 10);
			break;
		case 'y':
			if ((!optarg) && (optind < argc) &&
			    (argv[optind] != NULL) &&
			    (argv[optind][0] != '-')) {
				SETSTRING(channel_data_defaults.proxy,
					  argv[optind++]);
				break;
			}
			if (channel_data_defaults.proxy == NULL) {
				if ((getenv("http_proxy") == NULL) &&
				    (getenv("https_proxy") == NULL) &&
				    (getenv("HTTPS_PROXY") == NULL) &&
				    (getenv("ALL_PROXY") == NULL)) {
					ERROR("Should use proxy but no "
					      "proxy environment "
					      "variables nor proxy URL "
					      "set.\n");
					return SERVER_EINIT;
				}
				channel_data_defaults.proxy =
				    USE_PROXY_ENV;
			}
			break;
		case 'w':
			channel_data_defaults.retry_sleep =
			    (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'f':
			SETSTRING(channel_data_defaults.iface, optarg);
			break;
		case '1':
			server_hawkbit.usetokentodwl = false;
			break;
		case '2':
			SETSTRING(server_hawkbit.cached_file, optarg);
			break;
		case 'm':
			server_hawkbit.initial_report_resend_period =
				(unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 's':
			channel_data_defaults.connection_timeout =
				(unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'a':
			if (optind >= argc)
				return SERVER_EINIT;

			if (dict_insert_value(&server_hawkbit.httpheaders,
						optarg,
						argv[optind++]) < 0)
				return SERVER_EINIT;
			break;
		case '3':
			if (optind >= argc)
				return SERVER_EINIT;

			if (dict_insert_value(&server_hawkbit.configdata,
						optarg,
						argv[optind++]) < 0)
				return SERVER_EINIT;
			break;
		case 'n':
			channel_data_defaults.max_download_speed =
				(unsigned int)ustrtoull(optarg, NULL, 10);
			break;
		/* Ignore not recognized options, they can be already parsed by the caller */
		case '?':
			break;
		/* Ignore the --server option which is already parsed by the caller. */
		case 'S':
			break;
		}
	}

	if (mandatory_argument_count != ALL_MANDATORY_SET) {
		fprintf(stderr, "Mandatory arguments missing!\n");
		suricatta_print_help();
		return SERVER_EINIT;
	}
	if (argc > optind) {
		fprintf(stderr, "Unused arguments.\n");
		suricatta_print_help();
		return SERVER_EINIT;
	}

	channel_data_defaults.headers_to_send = &server_hawkbit.httpheaders;

	if (channel_curl_init() != CHANNEL_OK)
		return SERVER_EINIT;

	/*
	 * Set hawkBit token header if any has been set via command line
	 * or configuration file. hawkBit considers just first occurrence
	 * of field name (Authorization) in the header, so swupdate doesn't
	 * accept target and gateway token at the same time
	 */
	if (server_hawkbit.targettoken != NULL && server_hawkbit.gatewaytoken != NULL) {
		fprintf(stderr, "Error: both target and gateway tokens have been provided, "
				"but just one at a time is supported.\n");
		exit(EXIT_FAILURE);
	}
	server_hawkbit_settoken("TargetToken", server_hawkbit.targettoken);
	server_hawkbit_settoken("GatewayToken", server_hawkbit.gatewaytoken);

	/*
	 * Allocate a channel to communicate with the server
	 */
	server_hawkbit.channel = channel_new();
	if (!server_hawkbit.channel)
		return SERVER_EINIT;

	if (server_hawkbit.channel->open(server_hawkbit.channel, &channel_data_defaults) != CHANNEL_OK) {
		(void)server_hawkbit.channel->close(server_hawkbit.channel);
		free(server_hawkbit.channel);
		return SERVER_EINIT;
	}
	/* If an update was performed, report its status to the hawkBit server
	 * prior to entering the main loop. May run indefinitely if server is
	 * unavailable.
	 */
	server_hawkbit.update_state = update_state;

	/*
	 * After a successful startup, a configData is always sent
	 * Prepare default values
	 */
	server_hawkbit.has_to_send_configData = true;

	/*
	 * The following loop might block for a long time, if server does
	 * not respond immediately. Therefore, report pending request on
	 * server status IPC.
	 */
	report_server_status(CHANNEL_REQUEST_PENDING);

	/*
	 * If in WAIT state, the updated was finished
	 * by an external process and we have to wait for it
	 */
	if (update_state != STATE_WAIT) {
		while (server_handle_initial_state(update_state) != SERVER_OK) {
			INFO("Sleeping for %ds until retrying...",
				server_hawkbit.initial_report_resend_period);
			sleep(server_hawkbit.initial_report_resend_period);
		}
	}

	return SERVER_OK;
}

static server_op_res_t server_stop(void)
{
	(void)server_hawkbit.channel->close(server_hawkbit.channel);
	free(server_hawkbit.channel);
	return SERVER_OK;
}

/*
 * IPC is to control the hawkBit's communication
 */

static server_op_res_t server_activation_ipc(ipc_message *msg)
{
	server_op_res_t result = SERVER_OK;
	update_state_t update_state = STATE_NOT_AVAILABLE;
	struct json_object *json_root;

	json_root = server_tokenize_msg(msg->data.procmsg.buf,
					sizeof(msg->data.procmsg.buf));
	if (!json_root)
		return SERVER_EERR;

	json_object *json_data = json_get_path_key(
	    json_root, (const char *[]){"id", NULL});

	int action_id = -1;
	if (json_data) {
		action_id = json_object_get_int(json_data);
	} else {
		get_action_id_from_env(&action_id);
	}

	if (action_id < 0) {
		ERROR("No action_id passed into JSON message and no action:_id in env");
		return SERVER_EERR;
	}

	json_data = json_get_path_key(
	    json_root, (const char *[]){"status", NULL});
	if (json_data == NULL) {
		ERROR("Got malformed JSON: Could not find field status");
		DEBUG("Got JSON: %s", json_object_to_json_string(json_data));
		return SERVER_EERR;
	}
	update_state = (unsigned int)*json_object_get_string(json_data);
	DEBUG("Got action_id %d status %c", action_id, update_state);

	const char *reply_result = json_get_value(json_root, "finished");
	const char *reply_execution = json_get_value(json_root, "execution");
	json_data = json_get_key(json_root, "details");

	if (!hawkbit_enum_check("finished", reply_result) ||
	    !hawkbit_enum_check("execution", reply_execution) ||
	    !is_valid_state(update_state)) {
		ERROR("Wrong values \"execution\" : %s, \"finished\" : %s , \"status\" : %c",
		       	reply_execution, reply_result, update_state);
		return  SERVER_EERR;
	}

	int numdetails = json_object_array_length(json_data);
	const char **details = (const char **)malloc((numdetails + 1) * (sizeof (char *)));
	if(!details)
		return SERVER_EERR;

	if (!numdetails)
		details[0] = "";
	else {
		int i;
		for (i = 0; i < numdetails; i++) {
			details[i] = json_object_get_string(json_object_array_get_idx(json_data, i));
			TRACE("Detail %d : %s", i, details[i]);
		}
	}

	channel_data_t channel_data = channel_data_defaults;
	int server_action_id;
	result =
	    server_get_deployment_info(server_hawkbit.channel, &channel_data, &server_action_id);

        if (result != SERVER_OK && result != SERVER_UPDATE_AVAILABLE &&
            result != SERVER_NO_UPDATE_AVAILABLE &&
            result != SERVER_UPDATE_CANCELED && result != SERVER_ID_REQUESTED) {
          DEBUG("Hawkbit is not accessible, bailing out (%d)", result);
          result = SERVER_EERR;
          goto cleanup;
        }

        server_op_res_t response = SERVER_OK;

	if (result == SERVER_UPDATE_CANCELED) {
		DEBUG("Acknowledging cancelled update.");
		(void)server_send_cancel_reply(server_hawkbit.channel, server_action_id);
	}

	if (action_id != server_action_id) {
		TRACE("Deployment changed on server: our id %d, on server %d",
			action_id, server_action_id);
	} else {
		response = handle_feedback(action_id, result, update_state, reply_result,
					   reply_execution,
					   numdetails == 0 ? 1 : numdetails, details);
	}
	/*
	 * Only in case of errors report respond with NAK else send ACK
	 * It is useful when there is no deployment (deployment might be deleted)
	 * and external process tries to send the information to backend again
	 */

	if ((response != SERVER_UPDATE_AVAILABLE) && (response != SERVER_OK))
		result = SERVER_EERR;
	else {
		server_hawkbit.update_state = STATE_OK;

		/*
		 * Save the state
		 */
		if ((result = save_state(STATE_OK)) != SERVER_OK) {
			ERROR("Error while resetting update state on persistent "
			"storage.\n");
		}
	}

	msg->data.procmsg.len = 0;

cleanup:
	free(details);

	return result;
}

static void server_set_additional_device_attributes_ipc(json_object* json_data)
{
	for (int i = 0; i < json_object_array_length(json_data); ++i) {
		json_object* attr_obj = json_object_array_get_idx(json_data, i);
		json_object* attr_key_data = json_get_path_key(attr_obj, (const char *[]){"name", NULL});
		json_object* attr_value_data = json_get_path_key(attr_obj, (const char *[]){"value", NULL});

		const char* key = json_object_get_string(attr_key_data);
		const char* value = json_object_get_string(attr_value_data);
		dict_set_value(&server_hawkbit.configdata, key, value);
	}
}

static server_op_res_t server_configuration_ipc(ipc_message *msg)
{
	struct json_object *json_root;
	unsigned int polling;
	json_object *json_data;

	json_root = server_tokenize_msg(msg->data.procmsg.buf,
					sizeof(msg->data.procmsg.buf));
	if (!json_root)
		return SERVER_EERR;

	json_data = json_get_path_key(
	    json_root, (const char *[]){"polling", NULL});
	pthread_mutex_lock(&ipc_lock);
	if (json_data) {
		polling = json_object_get_int(json_data);
		if (polling > 0) {
			server_hawkbit.polling_interval_from_server = false;
			server_hawkbit.polling_interval = polling;
		} else
			server_hawkbit.polling_interval_from_server = true;
	}

	json_data = json_get_path_key(
		json_root, (const char*[]){"identify", NULL});

	if (json_data) {
		server_set_additional_device_attributes_ipc(json_data);
		server_hawkbit.has_to_send_configData = true;
	}

	pthread_mutex_unlock(&ipc_lock);
	return SERVER_OK;
}

static server_op_res_t server_status_ipc(ipc_message *msg)
{
	struct timeval tv = {
		.tv_sec = server_hawkbit.server_status_time,
		.tv_usec = 0
	};

	pthread_mutex_lock(&ipc_lock);
	sprintf(msg->data.procmsg.buf,
		"{\"server\":{\"status\":%d,\"time\":\"%s\"}}",
		server_hawkbit.server_status,
		swupdate_time_iso8601(&tv));
	msg->data.procmsg.len = strlen(msg->data.procmsg.buf);
	pthread_mutex_unlock(&ipc_lock);

	return SERVER_OK;
}

static server_op_res_t server_ipc(ipc_message *msg)
{
	server_op_res_t result = SERVER_OK;

	switch (msg->data.procmsg.cmd) {
	case CMD_ACTIVATION:
		result = server_activation_ipc(msg);
		break;
	case CMD_CONFIG:
		result = server_configuration_ipc(msg);
		break;
	case CMD_GET_STATUS:
		result = server_status_ipc(msg);
		break;
	default:
		result = SERVER_EERR;
		break;
	}

	if (result == SERVER_EERR) {
		msg->type = NACK;
	} else
		msg->type = ACK;

	msg->data.procmsg.len = 0;

	return SERVER_OK;
}

server_t server_hawkbit_funcs = {
	.has_pending_action = &server_has_pending_action,
	.install_update = &server_install_update,
	.send_target_data = &server_send_target_data,
	.get_polling_interval = &server_get_polling_interval,
	.start = &server_start,
	.stop = &server_stop,
	.ipc = &server_ipc,
	.help = &server_print_help,
};

__attribute__((constructor))
static void register_server_hawkbit(void)
{
	register_server(SERVER_NAME, &server_hawkbit_funcs);
}
