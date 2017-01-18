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

#ifdef CONFIG_SURICATTA_STATE_CHOICE_UBOOT
#define EXPANDTOKL2(token) token
#define EXPANDTOK(token) EXPANDTOKL2(token)
#define STATE_KEY EXPANDTOK(CONFIG_SURICATTA_STATE_UBOOT)
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
 * These are used to check if all mandatory fields
 * are set
 */
#define TENANT_BIT	1
#define ID_BIT		2
#define URL_BIT		4
#define ALL_MANDATORY_SET	(TENANT_BIT | ID_BIT | URL_BIT)


/* Prototypes for "internal" functions */
/* Note that they're not `static` so that they're callable from unit tests. */
json_object *json_get_key(json_object *json_root, const char *key);
json_object *json_get_path_key(json_object *json_root, const char **json_path);
char *json_get_data_url(json_object *json_root, const char *key);
server_op_res_t map_channel_retcode(channel_op_res_t response);
server_op_res_t server_handle_initial_state(update_state_t stateovrrd);
int server_update_status_callback(ipc_message *msg);
int server_update_done_callback(RECOVERY_STATUS status);
server_op_res_t server_process_update_artifact(json_object *json_data_artifact,
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
			     const char *execution_status, const char *details);
server_op_res_t server_send_cancel_reply(const int action_id);

server_hawkbit_t server_hawkbit = {.url = NULL,
				   .polling_interval = DEFAULT_POLLING_INTERVAL,
				   .debug = false,
				   .device_id = NULL,
				   .tenant = NULL};

static channel_data_t channel_data_defaults = {.debug = false,
					       .retries = DEFAULT_RESUME_TRIES,
					       .retry_sleep =
						   DEFAULT_RESUME_DELAY,
					       .strictssl = true};

/* Prototypes for "public" functions */
server_op_res_t server_has_pending_action(int *action_id);
server_op_res_t server_stop(void);
server_op_res_t server_ipc(int fd);
server_op_res_t server_start(char *fname, int argc, char *argv[]);
server_op_res_t server_install_update(void);
server_op_res_t server_send_target_data(void);
unsigned int server_get_polling_interval(void);

json_object *json_get_key(json_object *json_root, const char *key)
{
	json_object *json_child;
	if (json_object_object_get_ex(json_root, key, &json_child)) {
		return json_child;
	}
	return NULL;
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

server_op_res_t server_send_cancel_reply(const int action_id)
{
	assert(server_hawkbit.url != NULL);
	assert(server_hawkbit.tenant != NULL);
	assert(server_hawkbit.device_id != NULL);

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
	channel_data_t channel_data_reply = channel_data_defaults;
	server_op_res_t result = SERVER_OK;
	char *url = NULL;
	char *json_reply_string = NULL;
	if (ENOMEM_ASPRINTF ==
	    asprintf(&url, "%s/%s/controller/v1/%s/cancelAction/%d/feedback",
		     server_hawkbit.url, server_hawkbit.tenant,
		     server_hawkbit.device_id, action_id)) {
		ERROR("hawkBit server reply cannot be sent because of OOM.\n");
		result = SERVER_EINIT;
		goto cleanup;
	}

	char fdate[15 + 1];
	time_t now = time(NULL) == (time_t)-1 ? 0 : time(NULL);
	(void)strftime(fdate, sizeof(fdate), "%Y%m%dT%H%M%S", localtime(&now));
	if (ENOMEM_ASPRINTF ==
	    asprintf(&json_reply_string, json_hawkbit_cancelation_feedback,
		     action_id, fdate, reply_status_result_finished.success,
		     reply_status_execution.closed,
		     "cancellation acknowledged.")) {
		ERROR("hawkBit server reply cannot be sent because of OOM.\n");
		result = SERVER_EINIT;
		goto cleanup;
	}
	channel_data_reply.url = url;
	channel_data_reply.json_string = json_reply_string;
	channel_data_reply.method = CHANNEL_POST;
	result = map_channel_retcode(channel.put((void *)&channel_data_reply));

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
	return result;
}

server_op_res_t
server_send_deployment_reply(const int action_id, const int job_cnt_max,
			     const int job_cnt_cur, const char *finished,
			     const char *execution_status, const char *details)
{
	assert(finished != NULL);
	assert(execution_status != NULL);
	assert(details != NULL);
	assert(server_hawkbit.url != NULL);
	assert(server_hawkbit.tenant != NULL);
	assert(server_hawkbit.device_id != NULL);

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
			"details" : [ "%s" ]
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
		     execution_status, details)) {
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
	result = map_channel_retcode(channel.put((void *)&channel_data));

cleanup:
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


server_op_res_t server_set_config_data(json_object *json_root)
{
	char *tmp;

	tmp = json_get_data_url(json_root, "configData");

	if (tmp != NULL) {
		if (server_hawkbit.configData_url)
			free(server_hawkbit.configData_url);
		server_hawkbit.configData_url = tmp;
		server_hawkbit.has_to_send_configData = true;
		server_hawkbit.polling_interval /= 10;
		TRACE("ConfigData: %s\n", server_hawkbit.configData_url);
	}
	return SERVER_OK;
}

static server_op_res_t server_get_device_info(channel_data_t *channel_data)
{
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
	if ((result = map_channel_retcode(channel.get((void *)channel_data))) !=
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

static server_op_res_t server_get_deployment_info(channel_data_t *channel_data,
						  int *action_id)
{
	assert(channel_data != NULL);
	assert(action_id != NULL);
	server_op_res_t update_status = SERVER_OK;
	server_op_res_t result = SERVER_OK;
	char *url_deployment_base = NULL;
	char *url_cancel = NULL;
	channel_data_t channel_data_device_info = channel_data_defaults;
	if ((result = server_get_device_info(&channel_data_device_info)) !=
	    SERVER_OK) {
		goto cleanup;
	}
	if ((url_cancel = json_get_data_url(channel_data_device_info.json_reply,
					    "cancelAction")) != NULL) {
		update_status = SERVER_UPDATE_CANCELED;
		channel_data->url = url_cancel;
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
	if ((result = map_channel_retcode(channel.get((void *)channel_data))) !=
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

server_op_res_t server_has_pending_action(int *action_id)
{
	/*
	 * if configData was not yet sent,
	 * send it without asking for deviceInfo
	 */
	if (server_hawkbit.has_to_send_configData)
		return SERVER_ID_REQUESTED;

	channel_data_t channel_data = channel_data_defaults;
	server_op_res_t result =
	    server_get_deployment_info(&channel_data, action_id);
	/* TODO don't throw away reply JSON as it's fetched again by succinct
	 *      server_install_update() */
	if (channel_data.json_reply != NULL &&
	    json_object_put(channel_data.json_reply) != JSON_OBJECT_FREED) {
		ERROR("JSON object should be freed but was not.\n");
	}
	if (result == SERVER_UPDATE_CANCELED) {
		DEBUG("Acknowledging cancelled update.\n");
		(void)server_send_cancel_reply(*action_id);
		result = SERVER_OK;
	}
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

server_op_res_t server_handle_initial_state(update_state_t stateovrrd)
{
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

	int action_id;
	channel_data_t channel_data = channel_data_defaults;
	server_op_res_t result =
	    server_get_deployment_info(&channel_data, &action_id);
	switch (result) {
	case SERVER_OK:
	case SERVER_ID_REQUESTED:
	case SERVER_UPDATE_CANCELED:
	case SERVER_NO_UPDATE_AVAILABLE:
		DEBUG("No active update available, nothing to report to "
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

	DEBUG("Reporting Installation progress for ID %d: %s / %s / %s\n",
	      action_id, reply_result, reply_execution, reply_message);
	if (server_send_deployment_reply(action_id, 0, 0, reply_result,
					 reply_execution,
					 reply_message) != SERVER_OK) {
		ERROR("Error while reporting installation status to server.\n");
		return SERVER_EAGAIN;
	}

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

int server_update_status_callback(ipc_message __attribute__ ((__unused__)) *msg)
{
	/* NOTE The return code is actually irrelevant as it's not used by
	 *      `ipc_wait_for_complete()`. */
	/* TODO notify() status here to hawkBit or to some syslog service for
	 *      log persistency. */
	return 0;
}

server_op_res_t server_process_update_artifact(json_object *json_data_artifact,
						const char *update_action,
						const char *part,
						const char *version,
						const char *name)
{
	assert(json_data_artifact != NULL);
	assert(json_object_get_type(json_data_artifact) == json_type_array);
	server_op_res_t result = SERVER_OK;

	struct array_list *json_data_artifact_array =
	    json_object_get_array(json_data_artifact);
	int json_data_artifact_max =
	    json_object_array_length(json_data_artifact);
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
			ERROR("No artifact download HTTP URL reported by "
			      "server.\n");
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
			ERROR("No artifact download URL reported by server.\n");
			result = SERVER_EBADMSG;
			goto cleanup;
		}

		if (json_data_artifact_filename == NULL ||
		    json_data_artifact_sha1hash == NULL ||
		    json_data_artifact_size == NULL) {
			ERROR(
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
		"name": "%s"
		}
		);
		if (ENOMEM_ASPRINTF ==
		    asprintf(&channel_data.info, update_info,
			    update_action,
			    part,
			    version,
			    name)) {
			ERROR("hawkBit server reply cannot be sent because of OOM.\n");
			result = SERVER_EBADMSG;
			goto cleanup_loop;
		}

		channel_op_res_t cresult =
		    channel.get_file((void *)&channel_data);
		if ((result = map_channel_retcode(cresult)) != SERVER_OK) {
			ERROR("Error while downloading artifact.\n");
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
	return result;
}

server_op_res_t server_install_update(void)
{
	int action_id;
	channel_data_t channel_data = channel_data_defaults;
	server_op_res_t result =
	    server_get_deployment_info(&channel_data, &action_id);
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
		INFO("Update classified as to be 'skipped' by server.");
		if (server_send_deployment_reply(
			action_id, 0, 0, reply_status_result_finished.success,
			reply_status_execution.closed,
			(const char *)"Skipped Update.") != SERVER_OK) {
			ERROR("Error while reporting installation progress to "
			      "server.\n");
		}
		goto cleanup;
	}

	json_object *json_data_chunk =
	    json_get_path_key(channel_data.json_reply,
			      (const char *[]){"deployment", "chunks", NULL});
	if (json_data_chunk == NULL) {
		ERROR("Got malformed JSON: Could not find field "
		      "deployment->chunks.\n");
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
			ERROR("Got malformed JSON: Could not find fields "
			      "'part', 'version', or 'name'.\n");
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
			ERROR("Got malformed JSON: Could not find field "
			      "deployment->chunks->artifacts.\n");
			DEBUG("Got JSON: %s\n", json_object_to_json_string(
						    channel_data.json_reply));
			result = SERVER_EBADMSG;
			goto cleanup;
		}

		if (server_send_deployment_reply(
			action_id, json_data_chunk_max, json_data_chunk_count,
			reply_status_result_finished.none,
			reply_status_execution.proceeding,
			(const char *)"Installing Update Chunk Artifacts.") !=
		    SERVER_OK) {
			ERROR("Error while reporting installation progress to "
			      "server.\n");
			goto cleanup;
		}
		assert(json_object_get_type(json_data_chunk_artifacts) ==
		       json_type_array);
		result =
		    server_process_update_artifact(json_data_chunk_artifacts,
				json_object_get_string(json_deployment_update_action),
				json_object_get_string(json_data_chunk_part),
				json_object_get_string(json_data_chunk_version),
				json_object_get_string(json_data_chunk_name));

		if (result != SERVER_OK) {
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
			    reply_status_execution.closed,
			    (const char *)"Installing Update Chunk "
					  "Artifacts failed.");
			goto cleanup;
		}
		if (server_send_deployment_reply(
			action_id, json_data_chunk_max,
			json_data_chunk_count + 1,
			reply_status_result_finished.none,
			reply_status_execution.proceeding,
			(const char *)"Installed Chunk.") != SERVER_OK) {
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
		reply_status_execution.proceeding,
		(const char *)"All Chunks Installed.") != SERVER_OK) {
		ERROR("Error while reporting installation success to "
		      "server.\n");
	}

cleanup:
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
	struct dict_entry *entry;
	bool first = true;
	int len = 0;
	server_op_res_t result = SERVER_OK;

	LIST_FOREACH(entry, &server_hawkbit.configdata, next) {
		len += strlen(entry->varname) + strlen(entry->value) + strlen (" : ") + 6;
	}

	if (!len)
		return SERVER_OK;

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
	result = map_channel_retcode(channel.put((void *)&channel_data_reply));

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
	if (channel.open(&channel_data_defaults) != CHANNEL_OK) {
		return SERVER_EINIT;
	}
	/* If an update was performed, report its status to the hawkBit server
	 * prior to entering the main loop. May run indefinitely if server is
	 * unavailable. In case of an error, the error is returned to the main
	 * loop, thereby exiting suricatta. */
	server_op_res_t state_handled;
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

	return SERVER_OK;
}

server_op_res_t server_stop(void)
{
	(void)channel.close();
	return SERVER_OK;
}

server_op_res_t server_ipc(int fd)
{
	char buf[4096];
	int ret;

	ret = read(fd, buf, sizeof(buf));

	printf("%s read %d bytes\n", __func__, ret);

	return SERVER_OK;
}
