/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
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
#include "suricatta/suricatta.h"
#include "suricatta/channel.h"
#include "channel_hawkbit.h"
#include "suricatta/state.h"
#include "server_hawkbit.h"
#include "parselib.h"
#include "swupdate_settings.h"
#include "swupdate_dict.h"

#define DEFAULT_POLLING_INTERVAL 45
#define DEFAULT_RESUME_TRIES 5
#define DEFAULT_RESUME_DELAY 5
#define INITIAL_STATUS_REPORT_WAIT_DELAY 10

#define MAX_URL_LENGTH 2048
#define STRINGIFY(...) #__VA_ARGS__
#define JSON_OBJECT_FREED 1
#define ENOMEM_ASPRINTF -1

#ifdef CONFIG_SURICATTA_STATE_CHOICE_BOOTLOADER
#define EXPANDTOKL2(token) token
#define EXPANDTOK(token) EXPANDTOKL2(token)
#define STATE_KEY EXPANDTOK(CONFIG_SURICATTA_STATE_BOOTLOADER)
#else
#define STATE_KEY "none"
#endif

#define SETSTRING(p, v) do { \
	if (p) \
		free(p); \
	p = strdup(v); \
} while (0)

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
    {NULL, 0, NULL, 0}};

static unsigned short mandatory_argument_count = 0;

/*
 * See Hawkbit's API for an explanation
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


extern channel_op_res_t channel_hawkbit_init(void);
/* Prototypes for "internal" functions */
/* Note that they're not `static` so that they're callable from unit tests. */
json_object *json_get_key(json_object *json_root, const char *key);
const char *json_get_value(struct json_object *json_root,
			   const char *key);
json_object *json_get_path_key(json_object *json_root, const char **json_path);
char *json_get_data_url(json_object *json_root, const char *key);
server_op_res_t map_channel_retcode(channel_op_res_t response);
server_op_res_t server_handle_initial_state(update_state_t stateovrrd);
static int server_update_status_callback(ipc_message *msg);
int server_update_done_callback(RECOVERY_STATUS status);
server_op_res_t server_process_update_artifact(int action_id,
						json_object *json_data_artifact,
						const char *update_action,
						const char *part,
						const char *version,
						const char *name);
void suricatta_print_help(void);
server_op_res_t server_set_polling_interval(json_object *json_root);
server_op_res_t server_set_config_data(json_object *json_root);
static update_state_t get_state(void);
server_op_res_t
server_send_deployment_reply(const int action_id, const int job_cnt_max,
			     const int job_cnt_cur, const char *finished,
			     const char *execution_status, int numdetails, const char *details[]);
server_op_res_t server_send_cancel_reply(channel_t *channel, const int action_id);

server_hawkbit_t server_hawkbit = {.url = NULL,
				   .polling_interval = DEFAULT_POLLING_INTERVAL,
				   .polling_interval_from_server = true,
				   .debug = false,
				   .device_id = NULL,
				   .tenant = NULL,
				   .cancel_url = NULL,
				   .channel = NULL};

static channel_data_t channel_data_defaults = {.debug = false,
					       .retries = DEFAULT_RESUME_TRIES,
					       .retry_sleep =
						   DEFAULT_RESUME_DELAY,
					       .strictssl = true};

static struct timeval server_time;

/* Prototypes for "public" functions */
server_op_res_t server_has_pending_action(int *action_id);
server_op_res_t server_stop(void);
server_op_res_t server_ipc(int fd);
server_op_res_t server_start(char *fname, int argc, char *argv[]);
server_op_res_t server_install_update(void);
server_op_res_t server_send_target_data(void);
unsigned int server_get_polling_interval(void);

/*
 * This is called when a general error is found before sending the stream
 * to the installer. In this way, errors are collected in the same way as
 * when the installer is called.
 */
static inline void server_hawkbit_error(const char *s)
{
	int cnt = server_hawkbit.errorcnt;
	/* Store locally just the errors to send them back to hawkbit */
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

json_object *json_get_key(json_object *json_root, const char *key)
{
	json_object *json_child;
	if (json_object_object_get_ex(json_root, key, &json_child)) {
		return json_child;
	}
	return NULL;
}

const char *json_get_value(struct json_object *json_root,
			   const char *key)
{
	json_object *json_data = json_get_key(json_root, key);

	if (json_data == NULL)
		return "";

	return json_object_get_string(json_data);
}

json_object *json_get_path_key(json_object *json_root, const char **json_path)
{
	json_object *json_data = json_root;
	while (*json_path) {
		const char *key = *json_path;
		json_data = json_get_key(json_data, key);
		if (json_data == NULL) {
			return NULL;
		}
		json_path++;
	}
	return json_data;
}

char *json_get_data_url(json_object *json_root, const char *key)
{
	json_object *json_data = json_get_path_key(
	    json_root, (const char *[]){"_links", key, "href", NULL});
	return json_data == NULL
		   ? NULL
		   : strndup(json_object_get_string(json_data), MAX_URL_LENGTH);
}

server_op_res_t map_channel_retcode(channel_op_res_t response)
{
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
	case CHANNEL_OK:
		return SERVER_OK;
	}
	return SERVER_EERR;
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
		ERROR("hawkBit server reply cannot be sent because of OOM.\n");
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
		ERROR("hawkBit server reply cannot be sent because of OOM.\n");
		result = SERVER_EINIT;
		goto cleanup;
	}
	channel_data_reply.url = url;
	channel_data_reply.json_string = json_reply_string;
	channel_data_reply.method = CHANNEL_POST;
	result = map_channel_retcode(channel->put(channel, (void *)&channel_data_reply));

cleanup:
	if (url != NULL) {
		free(url);
	}
	if (json_reply_string != NULL) {
		free(json_reply_string);
	}
	if (channel_data_reply.json_reply != NULL &&
	    json_object_put(channel_data_reply.json_reply) !=
		JSON_OBJECT_FREED) {
		ERROR("JSON object should be freed but was not.\n");
	}

	/*
	 * Send always a notification
	 */
	char *notifybuf = NULL;
	if (ENOMEM_ASPRINTF ==
	    asprintf(&notifybuf, "{ \"id\" : \"%d\", \"stopId\" : \"%d\"}",
		     action_id, stop_id)) {
		notify(SUBPROCESS, CANCELUPDATE, "Update cancelled");
	}  else {
		notify(SUBPROCESS, CANCELUPDATE, notifybuf);
		free(notifybuf);
	}

	return result;
}

static char *server_create_details(int numdetails, const char *details[])
{
	int i, ret;
	char *prev = NULL;
	char *next = NULL;

	for (i = 0; i < numdetails; i++) {
		TRACE("Detail %d %s", i, details[i]);
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

	TRACE("Final details: %s\n", next);

	return next;
}

server_op_res_t
server_send_deployment_reply(const int action_id, const int job_cnt_max,
			     const int job_cnt_cur, const char *finished,
			     const char *execution_status, int numdetails, const char *details[])
{
	channel_t *channel = server_hawkbit.channel;
	assert(channel != NULL);
	assert(finished != NULL);
	assert(execution_status != NULL);
	assert(details != NULL);
	assert(server_hawkbit.url != NULL);
	assert(server_hawkbit.tenant != NULL);
	assert(server_hawkbit.device_id != NULL);

	char *detail = server_create_details(numdetails, details);

	TRACE("Reporting Installation progress for ID %d: %s / %s / %s \n",
	      action_id, finished, execution_status, detail);
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
		ERROR("hawkBit server reply cannot be sent because of OOM.\n");
		result = SERVER_EINIT;
		goto cleanup;
	}
	if (ENOMEM_ASPRINTF ==
	    asprintf(&url, "%s/%s/controller/v1/%s/deploymentBase/%d/feedback",
		     server_hawkbit.url, server_hawkbit.tenant,
		     server_hawkbit.device_id, action_id)) {
		ERROR("hawkBit server reply cannot be sent because of OOM.\n");
		result = SERVER_EINIT;
		goto cleanup;
	}
	channel_data.url = url;
	channel_data.json_string = json_reply_string;
	TRACE("PUTing to %s: %s\n", channel_data.url, channel_data.json_string);
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
	if (channel_data.json_reply != NULL &&
	    json_object_put(channel_data.json_reply) != JSON_OBJECT_FREED) {
		ERROR("JSON object should be freed but was not.\n");
	}
	return result;
}

server_op_res_t server_set_polling_interval(json_object *json_root)
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
		      "config->polling->sleep.\n");
		DEBUG("Got JSON: %s\n", json_object_to_json_string(json_data));
		return SERVER_EBADMSG;
	}
	static struct tm timedate;
	if (strptime(json_object_get_string(json_data), "%H:%M:%S",
		     &timedate) == NULL) {
		ERROR("Got malformed JSON: Could not convert field "
		      "config->polling->sleep to int.\n");
		DEBUG("Got JSON: %s\n", json_object_to_json_string(json_data));
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
	    polling_interval == 0 ? DEFAULT_POLLING_INTERVAL : polling_interval;
	DEBUG("Set polling interval to %ds as announced by server.\n",
	      server_hawkbit.polling_interval);
	return SERVER_OK;
}

unsigned int server_get_polling_interval(void)
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
		if (server_hawkbit.configData_url)
			free(server_hawkbit.configData_url);
		server_hawkbit.configData_url = tmp;
		server_hawkbit.has_to_send_configData = true;
		TRACE("ConfigData: %s\n", server_hawkbit.configData_url);
	}
	return SERVER_OK;
}

static server_op_res_t server_get_device_info(channel_t *channel, channel_data_t *channel_data)
{
	assert(channel != NULL);
	assert(channel_data != NULL);
	assert(server_hawkbit.url != NULL);
	assert(server_hawkbit.tenant != NULL);
	assert(server_hawkbit.device_id != NULL);
	DEBUG("Getting information for device '%s'\n",
	      server_hawkbit.device_id);
	server_op_res_t result = SERVER_OK;
	if (ENOMEM_ASPRINTF ==
	    asprintf(&channel_data->url, "%s/%s/controller/v1/%s",
		     server_hawkbit.url, server_hawkbit.tenant,
		     server_hawkbit.device_id)) {
		ERROR("hawkBit server cannot be queried because of OOM.\n");
		result = SERVER_EINIT;
		goto cleanup;
	}
	if ((result = map_channel_retcode(channel->get(channel, (void *)channel_data))) !=
	    SERVER_OK) {
		goto cleanup;
	}
	if ((result = server_set_polling_interval(channel_data->json_reply)) !=
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
		TRACE("Cancel action available at %s\n", url_cancel);
	} else if ((url_deployment_base =
			json_get_data_url(channel_data_device_info.json_reply,
					  "deploymentBase")) != NULL) {
		update_status = SERVER_UPDATE_AVAILABLE;
		channel_data->url = url_deployment_base;
		TRACE("Update action available at %s\n", url_deployment_base);
	} else {
		TRACE("No pending action on server.\n");
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
		ERROR("Got malformed JSON: Could not find field 'id'.\n");
		DEBUG("Got JSON: %s\n",
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
			ERROR("Got malformed JSON: Could not find field 'stopId', reuse actionId.\n");
			DEBUG("Got JSON: %s\n",
			      json_object_to_json_string(channel_data->json_reply));
		} else {
			server_hawkbit.stop_id = json_object_get_int(json_data);
		}
	}
	TRACE("Associated Action ID for Update Action is %d\n", *action_id);
	result = update_status == SERVER_OK ? result : update_status;

cleanup:
	if (channel_data_device_info.json_reply != NULL &&
	    json_object_put(channel_data_device_info.json_reply) !=
		JSON_OBJECT_FREED) {
		ERROR("JSON object should be freed but was not.\n");
	}
	if (url_cancel != NULL) {
		free(url_cancel);
	}
	if (url_deployment_base != NULL) {
		free(url_deployment_base);
	}
	return result;
}

static int server_check_during_dwl(void)
{
	struct timeval now;
	channel_data_t channel_data = channel_data_defaults;
	int action_id;
	int ret = 0;

	server_get_current_time(&now);

	/*
	 * The download can take a very long time
	 * In the meantime, check with current polling time
	 * if something on the server was changed and a cancel
	 * was requested
	 */
	if ((now.tv_sec - server_time.tv_sec) < ((int)server_get_polling_interval()))
		return 0;

	/* Update current server time */
	server_time = now;

	/*
	 * We need a separate channel because we want to run
	 * a connection parallel to the download
	 */
	channel_t *channel = channel_new();

	if (channel->open(channel, &channel_data_defaults) != CHANNEL_OK) {
		/*
		 * it is not possible to check for a cancelUpdate,
		 * go on downloading
		 */
		free(channel);
		return 0;
	}

	/*
	 * Send a device Info to the Hawkbit Server
	 */
	server_op_res_t result =
	    server_get_deployment_info(channel, &channel_data, &action_id);
	if (channel_data.json_reply != NULL &&
	    json_object_put(channel_data.json_reply) != JSON_OBJECT_FREED) {
		ERROR("JSON object should be freed but was not.\n");
	}
	if (result == SERVER_UPDATE_CANCELED) {
		/* Mark that an update was cancelled by the server */
		server_hawkbit.cancelDuringUpdate = true;
		ret = -1;
	}

	channel->close(channel);
	free(channel);

	return ret;
}

server_op_res_t server_has_pending_action(int *action_id)
{

	channel_data_t channel_data = channel_data_defaults;
	server_op_res_t result =
	    server_get_deployment_info(server_hawkbit.channel,
			    		&channel_data, action_id);
	/* TODO don't throw away reply JSON as it's fetched again by succinct
	 *      server_install_update() */
	if (channel_data.json_reply != NULL &&
	    json_object_put(channel_data.json_reply) != JSON_OBJECT_FREED) {
		ERROR("JSON object should be freed but was not.\n");
	}
	if (result == SERVER_UPDATE_CANCELED) {
		DEBUG("Acknowledging cancelled update.\n");
		(void)server_send_cancel_reply(server_hawkbit.channel, *action_id);
		/* Inform the installer that a CANCEL was received */
		return SERVER_OK;
	}

	/*
	 * First check if initialization was completed or
	 * a feedback should be sent to Hawkbit
	 */
	if (server_hawkbit.update_state == STATE_WAIT)
		return SERVER_OK;

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
		result = SERVER_NO_UPDATE_AVAILABLE;
	}

	return result;
}


static update_state_t get_state(void) {
	update_state_t state;

	if (read_state((char *)STATE_KEY, &state) != SERVER_OK) {
		ERROR("Cannot read stored update state.\n");
		return STATE_ERROR;
	}
	TRACE("Read state=%c from persistent storage.\n", state);

	return is_state_valid(state) ? state : STATE_ERROR;
}

static void add_detail_error(const char *s)
{
	int cnt = server_hawkbit.errorcnt;
	/* Store locally just the errors to send them back to hawkbit */
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

	if (server_send_deployment_reply(action_id, 0, 0, reply_result,
					 reply_execution,
					 numdetails, details) != SERVER_OK) {
		ERROR("Error while reporting installation status to server.\n");
		return SERVER_EAGAIN;
	}

	return SERVER_UPDATE_AVAILABLE;
}


server_op_res_t server_handle_initial_state(update_state_t stateovrrd)
{
	int action_id;
	update_state_t state = STATE_OK;
	if (stateovrrd != STATE_NOT_AVAILABLE) {
		state = stateovrrd;
		TRACE("Got state=%c from command line.\n", state);
		if (!is_state_valid(state)) {
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
	 * Retrieving current action id
	 */
	channel_data_t channel_data = channel_data_defaults;
	result =
	    server_get_deployment_info(server_hawkbit.channel, &channel_data, &action_id);

	result = handle_feedback(action_id, result, state, reply_result,
				 reply_execution, 1, &reply_message);

	if (result != SERVER_UPDATE_AVAILABLE)
		return result;

	/* NOTE (Re-)setting STATE_KEY=STATE_OK == '0' instead of deleting it
	 *      as it may be required for the switchback/recovery U-Boot logics.
	 */
	if ((result = save_state((char *)STATE_KEY, STATE_OK)) != SERVER_OK) {
		ERROR("Error while resetting update state on persistent "
		      "storage.\n");
		return result;
	}
	return SERVER_OK;
}

static int server_update_status_callback(ipc_message *msg)
{
	/* Store locally just the errors to send them back to hawkbit */
	add_detail_error(msg->data.status.desc);

	return 0;
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
	for (int json_data_artifact_count = 0;
	     json_data_artifact_count < json_data_artifact_max;
	     json_data_artifact_count++) {
		json_data_artifact_item = array_list_get_idx(
		    json_data_artifact_array, json_data_artifact_count);
		TRACE("Iterating over JSON, key=%s\n",
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
			DEBUG("Got JSON: %s\n", json_object_to_json_string(
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
			goto cleanup;
		}

		DEBUG("Processing '%s' from '%s'\n",
		      json_object_get_string(json_data_artifact_filename),
		      json_object_get_string(json_data_artifact_url));

		char *filename = NULL;
		channel_data_t channel_data = channel_data_defaults;
		channel_data.url =
		    strdup(json_object_get_string(json_data_artifact_url));

		static const char* const update_info = STRINGIFY(
		{
		"update": "%s",
		"part": "%s",
		"version": "%s",
		"name": "%s",
		"id" : "%d"
		}
		);
		if (ENOMEM_ASPRINTF ==
		    asprintf(&channel_data.info, update_info,
			    update_action,
			    part,
			    version,
			    name, action_id)) {
			ERROR("hawkBit server reply cannot be sent because of OOM.\n");
			result = SERVER_EBADMSG;
			goto cleanup_loop;
		}

		channel_data.checkdwl = server_check_during_dwl;

		/*
		 * Retrieve current time to check download time
		 * This is used in the callback to ask again the hawkbit
		 * server if the download is longer as the polling time
		 */

		server_get_current_time(&server_time);

		channel_op_res_t cresult =
		    channel->get_file(channel, (void *)&channel_data, 0);
		if ((result = map_channel_retcode(cresult)) != SERVER_OK) {
			/* this is called to collect errors */
			ipc_wait_for_complete(server_update_status_callback);
			goto cleanup_loop;
		}

#ifdef CONFIG_SURICATTA_SSL
		if (strncmp((char *)&channel_data.sha1hash,
			    json_object_get_string(json_data_artifact_sha1hash),
			    SHA_DIGEST_LENGTH) != 0) {
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
		if (channel_data.url != NULL) {
			free(channel_data.url);
		}
		if (filename != NULL) {
			free(filename);
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

	return result;
}

server_op_res_t server_install_update(void)
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
	json_object *json_deployment_update_action =
	    json_get_path_key(channel_data.json_reply,
			      (const char *[]){"deployment", "update", NULL});
	assert(json_object_get_type(json_deployment_update_action) ==
	       json_type_string);
	if (strncmp(json_object_get_string(json_deployment_update_action),
		    deployment_update_action.forced,
		    strlen(deployment_update_action.forced)) == 0) {
		INFO("Update classified as 'FORCED' by server.");
	} else if (strncmp(
		       json_object_get_string(json_deployment_update_action),
		       deployment_update_action.attempt,
		       strlen(deployment_update_action.attempt)) == 0) {
		INFO("Update classified as 'attempt' by server.");
	} else if (strncmp(
		       json_object_get_string(json_deployment_update_action),
		       deployment_update_action.skip,
		       strlen(deployment_update_action.skip)) == 0) {
		const char *details = "Skipped Update.";
		INFO("Update classified as to be 'skipped' by server.");
		if (server_send_deployment_reply(
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
		DEBUG("Got JSON: %s\n",
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
		TRACE("Iterating over JSON, key=%s\n",
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
			DEBUG("Got JSON: %s\n", json_object_to_json_string(
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
		DEBUG("Processing Update Chunk '%s', version %s, part %s\n",
		      json_object_get_string(json_data_chunk_name),
		      json_object_get_string(json_data_chunk_version),
		      json_object_get_string(json_data_chunk_part));

		json_object *json_data_chunk_artifacts = json_get_path_key(
		    json_data_chunk_item, (const char *[]){"artifacts", NULL});
		if (json_data_chunk_artifacts == NULL) {
			server_hawkbit_error("Got malformed JSON: Could not find field "
			      "deployment->chunks->artifacts.");
			DEBUG("Got JSON: %s\n", json_object_to_json_string(
						    channel_data.json_reply));
			result = SERVER_EBADMSG;
			goto cleanup;
		}

		if (server_send_deployment_reply(
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
				json_object_get_string(json_deployment_update_action),
				json_object_get_string(json_data_chunk_part),
				json_object_get_string(json_data_chunk_version),
				json_object_get_string(json_data_chunk_name));

		if (result != SERVER_OK) {

			/* Check if failed because it was cancelled */
			if (server_hawkbit.cancelDuringUpdate) {
				TRACE("Acknowledging cancelled update.\n");
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
			 	    action_id, json_data_chunk_max,
				    json_data_chunk_count,
				    reply_status_result_finished.failure,
				    reply_status_execution.closed, server_hawkbit.errorcnt,
				    (const char **)server_hawkbit.errors);
			}
			goto cleanup;
		}
		if (server_send_deployment_reply(
			action_id, json_data_chunk_max,
			json_data_chunk_count + 1,
			reply_status_result_finished.none,
			reply_status_execution.proceeding, 1,
			&details[2]) != SERVER_OK) {
			ERROR("Error while reporting installation progress to "
			      "server.\n");
		}
	}

	if ((result = save_state((char *)STATE_KEY, STATE_INSTALLED)) !=
	    SERVER_OK) {
		ERROR("Cannot persistently store update state.\n");
		goto cleanup;
	}

	if (server_send_deployment_reply(
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
		ERROR("JSON object should be freed but was not.\n");
	}
	if (result == SERVER_OK) {
		INFO("Update successful, executing post-update actions.\n");
		ipc_message msg;
		if (ipc_postupdate(&msg) != 0) {
			result = SERVER_EERR;
		} else {
			result = msg.type == ACK ? SERVER_OK : SERVER_EERR;
			DEBUG("%s\n", msg.data.msg);
		}
	}
	return result;
}

server_op_res_t server_send_target_data(void)
{
	channel_t *channel = server_hawkbit.channel;
	struct dict_entry *entry;
	bool first = true;
	int len = 0;
	server_op_res_t result = SERVER_OK;

	assert(channel != NULL);
	LIST_FOREACH(entry, &server_hawkbit.configdata, next) {
		len += strlen(entry->varname) + strlen(entry->value) + strlen (" : ") + 6;
	}

	if (!len) {
		server_hawkbit.has_to_send_configData = false;
		return SERVER_OK;
	}

	char *configData = (char *)(malloc(len + 16));
	memset(configData, 0, len + 16);

	static const char* const config_data = STRINGIFY(
		%c"%s": "%s"
	);

	char *keyvalue = NULL;
	LIST_FOREACH(entry, &server_hawkbit.configdata, next) {
		if (ENOMEM_ASPRINTF ==
		    asprintf(&keyvalue, config_data,
				((first) ? ' ' : ','),
				entry->varname,
				entry->value)) {
			ERROR("hawkBit server reply cannot be sent because of OOM.\n");
			result = SERVER_EINIT;
			goto cleanup;
		}
		first = false;
		TRACE("KEYVALUE=%s %s %s", keyvalue, entry->varname, entry->value);
		strcat(configData, keyvalue);
		free(keyvalue);

	}

	TRACE("CONFIGDATA=%s", configData);

	static const char* const json_hawkbit_config_data = STRINGIFY(
	{
		"id": "%s",
		"time": "%s",
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

	char *url = NULL;
	char *json_reply_string = NULL;
	channel_data_t channel_data_reply = channel_data_defaults;
	char fdate[15 + 1];
	time_t now = time(NULL) == (time_t)-1 ? 0 : time(NULL);
	(void)strftime(fdate, sizeof(fdate), "%Y%m%dT%H%M%S", localtime(&now));

	if (ENOMEM_ASPRINTF ==
	    asprintf(&json_reply_string, json_hawkbit_config_data,
		     "", fdate, reply_status_result_finished.success,
		     reply_status_execution.closed,
		     "", configData)) {
		ERROR("hawkBit server reply cannot be sent because of OOM.\n");
		result = SERVER_EINIT;
		goto cleanup;
	}
	if (ENOMEM_ASPRINTF ==
	    asprintf(&url, "%s/%s/controller/v1/%s/configData",
		     server_hawkbit.url, server_hawkbit.tenant,
		     server_hawkbit.device_id)) {
		ERROR("hawkBit server reply cannot be sent because of OOM.\n");
		result = SERVER_EINIT;
		goto cleanup;
	}

	channel_data_reply.url = url;
	channel_data_reply.json_string = json_reply_string;
	TRACE("URL=%s JSON=%s", channel_data_reply.url, channel_data_reply.json_string);
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

void suricatta_print_help(void)
{
	fprintf(
	    stderr,
	    "\tsuricatta arguments (mandatory arguments are marked with '*'):\n"
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
	    "{http,all}_proxy env is tried.\n",
	    DEFAULT_POLLING_INTERVAL, DEFAULT_RESUME_TRIES,
	    DEFAULT_RESUME_DELAY);
}

static int suricatta_settings(void *elem, void  __attribute__ ((__unused__)) *data)
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

	get_field(LIBCFG_PARSER, elem, "retry",
		&channel_data_defaults.retries);

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "retrywait", tmp);
	if (strlen(tmp))
		channel_data_defaults.retry_sleep =
			(unsigned int)strtoul(tmp, NULL, 10);
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "cafile", tmp);
	if (strlen(tmp))
		SETSTRING(channel_data_defaults.cafile, tmp);
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "sslkey", tmp);
	if (strlen(tmp))
		SETSTRING(channel_data_defaults.sslkey, tmp);
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "sslcert", tmp);
	if (strlen(tmp))
		SETSTRING(channel_data_defaults.sslcert, tmp);
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "proxy", tmp);
	if (strlen(tmp))
		SETSTRING(channel_data_defaults.proxy, tmp);

	return 0;

}

static int suricatta_configdata_settings(void *settings, void  __attribute__ ((__unused__)) *data)
{
	void *elem;
	int count, i;
	char name[80], value[80];

	count = get_array_length(LIBCFG_PARSER, settings);

	for(i = 0; i < count; ++i) {
		elem = get_elem_from_idx(LIBCFG_PARSER, settings, i);

		if (!elem)
			continue;

		if(!(exist_field_string(LIBCFG_PARSER, elem, "name")))
			continue;
		if(!(exist_field_string(LIBCFG_PARSER, elem, "value")))
			continue;

		GET_FIELD_STRING(LIBCFG_PARSER, elem, "name", name);
		GET_FIELD_STRING(LIBCFG_PARSER, elem, "value", value);
		dict_set_value(&server_hawkbit.configdata, name, value);
		TRACE("Identify for configData: %s --> %s\n",
				name, value);
	}

	return 0;
}

server_op_res_t server_start(char *fname, int argc, char *argv[])
{
	update_state_t update_state = STATE_NOT_AVAILABLE;
	int choice = 0;

	mandatory_argument_count = 0;

	LIST_INIT(&server_hawkbit.configdata);

	if (fname) {
		read_module_settings(fname, "suricatta", suricatta_settings,
					NULL);
		read_module_settings(fname, "identify", suricatta_configdata_settings,
					NULL);
	}

	if (loglevel >= DEBUGLEVEL) {
		server_hawkbit.debug = true;
	}
	if (loglevel >= TRACELEVEL) {
		channel_data_defaults.debug = true;
	}

	/* reset to optind=1 to parse suricatta's argument vector */
	optind = 1;
	while ((choice = getopt_long(argc, argv, "t:i:c:u:p:xr:y::w:",
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
		case 'c':
			/* When no persistent update state storage is available,
			 * use command line switch to instruct what to report.
			 */
			update_state = (unsigned int)*optarg;
			switch (update_state) {
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
				    (getenv("all_proxy") == NULL)) {
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
		case '?':
		default:
			return SERVER_EINIT;
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

	if (channel_hawkbit_init() != CHANNEL_OK)
		return SERVER_EINIT;

	/*
	 * Allocate a channel to communicate with the server
	 */
	server_hawkbit.channel = channel_new();
	if (!server_hawkbit.channel)
		return SERVER_EINIT;

	if (server_hawkbit.channel->open(server_hawkbit.channel, &channel_data_defaults) != CHANNEL_OK) {
		return SERVER_EINIT;
	}
	/* If an update was performed, report its status to the hawkBit server
	 * prior to entering the main loop. May run indefinitely if server is
	 * unavailable. In case of an error, the error is returned to the main
	 * loop, thereby exiting suricatta. */
	server_op_res_t state_handled;
	server_hawkbit.update_state = update_state;


	/*
	 * After a successful startup, a configData is always sent
	 * Prepare default values
	 */
	server_hawkbit.has_to_send_configData = true;

	/*
	 * If in WAIT state, the updated was finished
	 * by an external process and we have to wait for it
	 */
	if (update_state != STATE_WAIT) {
		while ((state_handled = server_handle_initial_state(update_state)) !=
		       SERVER_OK) {
			if (state_handled == SERVER_EAGAIN) {
				INFO("Sleeping for %ds until retrying...\n",
				     INITIAL_STATUS_REPORT_WAIT_DELAY);
				sleep(INITIAL_STATUS_REPORT_WAIT_DELAY);
				continue;
			}
			return state_handled; /* Report error to main loop, exiting. */
		}
	}

	return SERVER_OK;
}

server_op_res_t server_stop(void)
{
	(void)server_hawkbit.channel->close(server_hawkbit.channel);
	return SERVER_OK;
}

/*
 * IPC is to control the Hawkbit's communication
 */

static struct json_object *server_tokenize_msg(char *buf, size_t size)
{

	struct json_tokener *json_tokenizer = json_tokener_new();
	enum json_tokener_error json_res;
	struct json_object *json_root;
	do {
		json_root = json_tokener_parse_ex(
		    json_tokenizer, buf, size);
	} while ((json_res = json_tokener_get_error(json_tokenizer)) ==
		 json_tokener_continue);
	if (json_res != json_tokener_success) {
		ERROR("Error while parsing channel's returned JSON data: %s\n",
		      json_tokener_error_desc(json_res));
		json_tokener_free(json_tokenizer);
		return NULL;
	}

	json_tokener_free(json_tokenizer);

	return json_root;
}

static server_op_res_t server_activation_ipc(ipc_message *msg)
{
	server_op_res_t result = SERVER_OK;
	update_state_t update_state = STATE_NOT_AVAILABLE;
	struct json_object *json_root;

	json_root = server_tokenize_msg(msg->data.instmsg.buf,
					sizeof(msg->data.instmsg.buf));
	if (!json_root)
		return SERVER_EERR;

	json_object *json_data = json_get_path_key(
	    json_root, (const char *[]){"id", NULL});
	if (json_data == NULL) {
		ERROR("Got malformed JSON: Could not find action id");
		DEBUG("Got JSON: %s\n", json_object_to_json_string(json_data));
		return SERVER_EERR;
	}
	int action_id = json_object_get_int(json_data);

	json_data = json_get_path_key(
	    json_root, (const char *[]){"status", NULL});
	if (json_data == NULL) {
		ERROR("Got malformed JSON: Could not find field status");
		DEBUG("Got JSON: %s\n", json_object_to_json_string(json_data));
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

	server_op_res_t response = SERVER_OK;
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
		server_hawkbit.update_state = SERVER_OK;

		/*
		 * Save the state
		 */
		save_state((char *)STATE_KEY, STATE_OK);
	}

	msg->data.instmsg.len = 0;

	return result;
}

static server_op_res_t server_configuration_ipc(ipc_message *msg)
{
	struct json_object *json_root;
	unsigned int polling;
	json_object *json_data;

	json_root = server_tokenize_msg(msg->data.instmsg.buf,
					sizeof(msg->data.instmsg.buf));
	if (!json_root)
		return SERVER_EERR;

	json_data = json_get_path_key(
	    json_root, (const char *[]){"polling", NULL});
	if (json_data) {
		polling = json_object_get_int(json_data);
		if (polling > 0) {
			server_hawkbit.polling_interval_from_server = false;
			server_hawkbit.polling_interval = polling;
		} else
			server_hawkbit.polling_interval_from_server = true;
	}

	return SERVER_OK;
}

server_op_res_t server_ipc(int fd)
{
	ipc_message msg;
	server_op_res_t result = SERVER_OK;
	int ret;

	ret = read(fd, &msg, sizeof(msg));
	if (ret != sizeof(msg))
		return SERVER_EERR;

	switch (msg.data.instmsg.cmd) {
	case CMD_ACTIVATION:
		result = server_activation_ipc(&msg);
		break;
	case CMD_CONFIG:
		result = server_configuration_ipc(&msg);
		break;
	default:
		result = SERVER_EERR;
		break;
	}

	if (result == SERVER_EERR) {
		msg.type = NACK;
	} else
		msg.type = ACK;

	msg.data.instmsg.len = 0;

	if (write(fd, &msg, sizeof(msg)) != sizeof(msg)) {
		TRACE("IPC ERROR: sending back msg");
	}

	/* Send ipc back */

	return SERVER_OK;
}
