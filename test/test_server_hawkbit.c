/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <sys/types.h>
#include <unistd.h>
#include <cmocka.h>
#include <network_ipc.h>
#include <swupdate_status.h>
#include <util.h>
#include "pctl.h"
#include "suricatta/suricatta.h"
#include "../suricatta/server_hawkbit.h"
#include "channel.h"
#include "channel_curl.h"
#include "state.h"

#define JSON_OBJECT_FREED 1
#define JSONQUOTE(...) #__VA_ARGS__

/* Keep in sync with corelib/channel_curl.c's prototypes for public functions. */
extern channel_op_res_t channel_close(channel_t *this);
extern channel_op_res_t channel_open(channel_t *this, void *cfg);
extern channel_op_res_t channel_put(channel_t *this, void *data);
extern channel_op_res_t channel_get_file(channel_t *this, void *data);
extern channel_op_res_t channel_get(channel_t *this, void *data);
extern channel_op_res_t channel_curl_init(void);

extern json_object *json_get_key(json_object *json_root, const char *key);

extern int __real_ipc_wait_for_complete(getstatus callback);
int __wrap_ipc_wait_for_complete(getstatus callback);
int __wrap_ipc_wait_for_complete(getstatus callback)
{
	(void)callback;
	return mock_type(RECOVERY_STATUS);
}

extern int __real_ipc_postupdate(ipc_message *msg);
int __wrap_ipc_postupdate(ipc_message *msg);
int __wrap_ipc_postupdate(ipc_message *msg) {
	msg->type = ACK;
	return 0;
}

extern channel_op_res_t __real_channel_open(channel_t *this, void *cfg);
channel_op_res_t __wrap_channel_open(channel_t *this, void *cfg);
channel_op_res_t __wrap_channel_open(channel_t *this, void *cfg)
{
	(void)this;
	(void)cfg;
	return mock_type(channel_op_res_t);
}

extern channel_op_res_t __real_channel_close(channel_t *this);
channel_op_res_t __wrap_channel_close(channel_t *this);
channel_op_res_t __wrap_channel_close(channel_t *this)
{
	(void)this;
	return mock_type(channel_op_res_t);
}

extern channel_op_res_t __real_channel_put(channel_t *this, void *data);
channel_op_res_t __wrap_channel_put(channel_t *this, void *data);
channel_op_res_t __wrap_channel_put(channel_t *this, void *data)
{
	(void)data;
	(void)this;
	return mock_type(channel_op_res_t);
}

extern channel_op_res_t __real_channel_get_file(channel_t *this, void *data);
channel_op_res_t __wrap_channel_get_file(channel_t *this, void *data);
channel_op_res_t __wrap_channel_get_file(channel_t *this, void *data)
{
#ifdef CONFIG_SURICATTA_SSL
	channel_data_t *channel_data = (channel_data_t *)data;
	strncpy(channel_data->sha1hash, mock_type(char *),
		SWUPDATE_SHA_DIGEST_LENGTH * 2 + 1);
#else
	(void)data;
#endif
	(void)this;
	return mock_type(channel_op_res_t);
}

extern channel_op_res_t __real_channel_get(channel_t *this, void *data);
channel_op_res_t __wrap_channel_get(channel_t *this, void *data);
channel_op_res_t __wrap_channel_get(channel_t *this, void *data)
{
	(void)this;
	channel_data_t *channel_data = (channel_data_t *)data;
	channel_data->json_reply = mock_ptr_type(json_object *);
	return mock_type(channel_op_res_t);
}

extern int __real_save_state(update_state_t value);
int __wrap_save_state(update_state_t *value);
int __wrap_save_state(update_state_t *value)
{
	(void)value;
	return mock_type(int);
}

extern update_state_t __real_get_state(void);
update_state_t __wrap_get_state(void);
update_state_t __wrap_get_state(void)
{
	return mock_type(update_state_t);
}

extern server_op_res_t server_has_pending_action(int *action_id);
static void test_server_has_pending_action(void **state)
{
	(void)state;

	/* clang-format off */
	static const char *json_reply_no_update = JSONQUOTE(
	{
		"config" : {
			"polling" : {
				"sleep" : "00:01:00"
			}
		}
	}
	);
	static const char *json_reply_update_available = JSONQUOTE(
	{
		"config" : {
			"polling" : {
				"sleep" : "00:01:00"
			}
		},
		"_links" : {
			"deploymentBase" : {
				"href" : "http://deploymentBase"
			}
		}
	}
	);
	static const char *json_reply_update_data = JSONQUOTE(
	{
		"id" : "12",
		"deployment" : {
			"download" : "forced",
			"update" : "forced",
			"chunks" : [
				{
					"part" : "part01",
					"version" : "v1.0.77",
					"name" : "oneapplication",
					"artifacts" : ["list of artifacts"]
				}
			]
		}
	}
	);
	static const char *json_reply_cancel_available = JSONQUOTE(
	{
		"config" : {
			"polling" : {
				"sleep" : "00:01:00"
			}
		},
		"_links" : {
			"cancelAction" : {
				"href" : "http://cancelAction"
			}
		}
	}
	);
	static const char *json_reply_cancel_data = JSONQUOTE(
	{
		"id" : "5",
		"cancelAction" : {
			"stopId" : "5"
		}
	}
	);
	/* clang-format on */

	/* Test Case: No Action available. */
	int action_id;
	will_return(__wrap_channel_get,
		    json_tokener_parse(json_reply_no_update));
	will_return(__wrap_channel_get, CHANNEL_OK);
	assert_int_equal(SERVER_NO_UPDATE_AVAILABLE,
			 server_has_pending_action(&action_id));

	/* Test Case: Update Action available && !STATE_INSTALLED. */
	will_return(__wrap_channel_get,
		    json_tokener_parse(json_reply_update_available));
	will_return(__wrap_channel_get, CHANNEL_OK);
	will_return(__wrap_channel_get,
		    json_tokener_parse(json_reply_update_data));
	will_return(__wrap_channel_get, CHANNEL_OK);
	will_return(__wrap_get_state, STATE_NOT_AVAILABLE);
	assert_int_equal(SERVER_UPDATE_AVAILABLE,
			 server_has_pending_action(&action_id));

	/* Test Case: Update Action available && STATE_INSTALLED. */
	will_return(__wrap_channel_get,
		    json_tokener_parse(json_reply_update_available));
	will_return(__wrap_channel_get, CHANNEL_OK);
	will_return(__wrap_channel_get,
		    json_tokener_parse(json_reply_update_data));
	will_return(__wrap_channel_get, CHANNEL_OK);
	will_return(__wrap_get_state, STATE_INSTALLED);
	assert_int_equal(SERVER_NO_UPDATE_AVAILABLE,
			 server_has_pending_action(&action_id));

	/* Test Case: Cancel Action available. */
	will_return(__wrap_channel_get,
		    json_tokener_parse(json_reply_cancel_available));
	will_return(__wrap_channel_get, CHANNEL_OK);
	will_return(__wrap_channel_get,
		    json_tokener_parse(json_reply_cancel_data));
	will_return(__wrap_channel_get, CHANNEL_OK);
	will_return(__wrap_channel_put, CHANNEL_OK);
	will_return(__wrap_save_state, 0);
	assert_int_equal(SERVER_OK, server_has_pending_action(&action_id));
}

extern server_op_res_t server_set_polling_interval_json(json_object *json_root);
static void test_server_set_polling_interval_json(void **state)
{
	(void)state;

	/* clang-format off */
	static const char *json_string_valid = JSONQUOTE(
	{
		"config" : {
			"polling" : {
				"sleep" : "00:01:00"
			}
		}
	}
	);
	static const char *json_string_invalid_time = JSONQUOTE(
	{
		"config" : {
			"polling" : {
				"sleep" : "XX:00:00"
			}
		}
	}
	);
	/* clang-format on */

	assert_int_equal(SERVER_EBADMSG, server_set_polling_interval_json(NULL));

	json_object *json_data = NULL;
	assert_non_null((json_data = json_tokener_parse(json_string_valid)));
	assert_int_equal(SERVER_OK, server_set_polling_interval_json(json_data));
	assert_int_equal(server_hawkbit.polling_interval, 60);
	assert_int_equal(json_object_put(json_data), JSON_OBJECT_FREED);
	json_data = NULL;

	assert_non_null(
	    (json_data = json_tokener_parse(json_string_invalid_time)));
	assert_int_equal(SERVER_EBADMSG,
			 server_set_polling_interval_json(json_data));
	assert_int_equal(json_object_put(json_data), JSON_OBJECT_FREED);
}

extern server_op_res_t
server_send_deployment_reply(channel_t *channel, const int action_id, const int job_cnt_max,
			     const int job_cnt_cur, const char *finished,
			     const char *execution_status, int numdetails, const char *details[]);
static void test_server_send_deployment_reply(void **state)
{
	(void)state;
	int action_id = 23;
	const char *details[1] = { "UNIT TEST" };

	/* Test Case: Channel sent reply. */
	will_return(__wrap_channel_put, CHANNEL_OK);
	assert_int_equal(SERVER_OK,
			 server_send_deployment_reply(
			     server_hawkbit.channel,
			     action_id, 5, 5,
			     reply_status_result_finished.success,
			     reply_status_execution.closed, 1, details));

	/* Test Case: Channel didn't sent reply. */
	will_return(__wrap_channel_put, CHANNEL_EIO);
	assert_int_equal(SERVER_EERR,
			 server_send_deployment_reply(
			     server_hawkbit.channel,
			     action_id, 5, 5,
			     reply_status_result_finished.success,
			     reply_status_execution.closed, 1, details));
}

extern server_op_res_t server_send_cancel_reply(channel_t *channel, const int action_id);
static void test_server_send_cancel_reply(void **state)
{
	(void)state;
	int action_id = 23;

	/* Test Case: Channel sent reply. */
	will_return(__wrap_channel_put, CHANNEL_OK);
	assert_int_equal(SERVER_OK, server_send_cancel_reply(server_hawkbit.channel, action_id));

	/* Test Case: Channel didn't sent reply. */
	will_return(__wrap_channel_put, CHANNEL_EIO);
	assert_int_equal(SERVER_EERR, server_send_cancel_reply(server_hawkbit.channel, action_id));
}

extern server_op_res_t
server_process_update_artifact(int action_id, json_object *json_data_artifact,
			       const char *update_action, const char *part,
			       const char *version, const char *name);

static void test_server_process_update_artifact(void **state)
{
	(void)state;
	int action_id = 23;
	/* clang-format off */
	static const char *json_artifact = JSONQUOTE(
	{
		"artifacts": [
		{
			"filename" : "afile.swu",
			"hashes" : {
				"sha1" : "CAFFEE",
				"md5" : "DEADBEEF",
			},
			"size" : 12,
			"_links" : {
				"download" : {
					"href" : "http://download"
				},
				"md5sum" : {
					"href" : "http://md5sum"
				}
			}
		}
		]
	}
	);
	/* clang-format on */

#ifndef CONFIG_SURICATTA_SSL
	/* Test Case: No HTTP download URL given in JSON. */
	json_object *json_data_artifact = json_tokener_parse(json_artifact);
	assert_int_equal(SERVER_EERR,
			 server_process_update_artifact(action_id,
			     json_get_key(json_data_artifact, "artifacts"),
			     "update action", "part", "version", "name"));
#endif

#ifdef CONFIG_SURICATTA_SSL
	/* Test Case: Artifact installed successfully. */
	json_object *json_data_artifact = json_tokener_parse(json_artifact);
	will_return(__wrap_channel_get_file, "CAFFEE");
	will_return(__wrap_channel_get_file, CHANNEL_OK);
	will_return(__wrap_ipc_wait_for_complete, SUCCESS);
	assert_int_equal(SERVER_OK,
			 server_process_update_artifact(action_id,
			     json_get_key(json_data_artifact, "artifacts"),
			     "update action", "part", "version", "name"));
	assert_int_equal(json_object_put(json_data_artifact),
			 JSON_OBJECT_FREED);
#endif
}

extern server_op_res_t server_install_update(void);
static void test_server_install_update(void **state)
{
	(void)state;

	/* clang-format off */
	static const char *json_reply_update_available = JSONQUOTE(
	{
		"config" : {
			"polling" : {
				"sleep" : "00:01:00"
			}
		},
		"_links" : {
			"deploymentBase" : {
				"href" : "http://deploymentBase"
			}
		}
	}
	);
	static const char *json_reply_update_invalid_data = JSONQUOTE(
	{
		"id" : "12",
		"deployment" : {
			"download" : "forced",
			"update" : "forced",
			"chunks" : [
				{
					"part" : "part01",
					"version" : "v1.0.77",
					"name" : "oneapplication",
					"artifacts" : ["no artifacts, failure"]
				}
			]
		}
	}
	);
	static const char *json_reply_update_valid_data_https = JSONQUOTE(
	{
		"id" : "12",
		"deployment" : {
		"download" : "forced",
		"update" : "forced",
		"chunks" : [
			{
			"part" : "part01",
			"version" : "v1.0.77",
			"name" : "oneapplication",
			"artifacts": [
				{
					"filename" : "afile.swu",
					"hashes" : {
						"sha1" : "CAFFEE",
						"md5" : "DEADBEEF",
					},
					"size" : 12,
					"_links" : {
						"download" : {
							"href" : "http://download"
						},
						"md5sum" : {
							"href" : "http://md5sum"
						}
					}
				}
			]
			}
		]
		}
	}
	);
	static const char *json_reply_update_valid_data_http = JSONQUOTE(
	{
		"id" : "12",
		"deployment" : {
		"download" : "forced",
		"update" : "forced",
		"chunks" : [
			{
			"part" : "part01",
			"version" : "v1.0.77",
			"name" : "oneapplication",
			"artifacts": [
				{
					"filename" : "afile.swu",
					"hashes" : {
						"sha1" : "CAFFEE",
						"md5" : "DEADBEEF",
					},
					"size" : 12,
					"_links" : {
						"download-http" : {
							"href" : "http://download"
						},
						"md5sum" : {
							"href" : "http://md5sum"
						}
					}
				}
			]
			}
		]
		}
	}
	);
	/* clang-format on */

	json_object *json_data_update_details_valid = NULL;
	json_object *json_data_update_available = NULL;
	json_object *json_data_update_details_invalid = NULL;

	/* Test Case: Update details are malformed JSON. */
	json_data_update_available =
	    json_tokener_parse(json_reply_update_available);
	json_data_update_details_invalid =
	    json_tokener_parse(json_reply_update_invalid_data);
	will_return(__wrap_channel_get, json_data_update_available);
	will_return(__wrap_channel_get, CHANNEL_OK);
	will_return(__wrap_channel_get, json_data_update_details_invalid);
	will_return(__wrap_channel_get, CHANNEL_OK);
	will_return(__wrap_channel_put, CHANNEL_OK);
	will_return(__wrap_channel_put, CHANNEL_OK);
	server_install_update();

	/* Test Case: Update works. */
	json_data_update_available =
	    json_tokener_parse(json_reply_update_available);
#ifdef CONFIG_SURICATTA_SSL
	json_data_update_details_valid =
	    json_tokener_parse(json_reply_update_valid_data_https);
	(void)json_reply_update_valid_data_http;
#else
	json_data_update_details_valid =
		json_tokener_parse(json_reply_update_valid_data_http);
	(void)json_reply_update_valid_data_https;
#endif
	will_return(__wrap_channel_get, json_data_update_available);
	will_return(__wrap_channel_get, CHANNEL_OK);
	will_return(__wrap_channel_get, json_data_update_details_valid);
	will_return(__wrap_channel_get, CHANNEL_OK);
	will_return(__wrap_channel_put, CHANNEL_OK);
#ifdef CONFIG_SURICATTA_SSL
	will_return(__wrap_channel_get_file, "CAFFEE");
#endif
	will_return(__wrap_channel_get_file, CHANNEL_OK);
	will_return(__wrap_ipc_wait_for_complete, SUCCESS);
	will_return(__wrap_channel_put, CHANNEL_OK);
	//will_return(__wrap_save_state, SERVER_OK);
	will_return(__wrap_channel_put, CHANNEL_OK);
	assert_int_equal(SERVER_OK, server_install_update());
}

static int server_hawkbit_setup(void **state)
{
	(void)state;
	server_hawkbit.url = (char *)"http://void.me";
	server_hawkbit.tenant = (char *)"tenant";
	server_hawkbit.device_id = (char *)"deviceID";
	server_hawkbit.channel = channel_new();
	server_hawkbit.channel->open = & channel_open;
	server_hawkbit.channel->close = & channel_close;
	server_hawkbit.channel->get = & channel_get;
	server_hawkbit.channel->get_file = & channel_get_file;
	server_hawkbit.channel->put = & channel_put;
	return 0;
}

static int server_hawkbit_teardown(void **state)
{
	(void)state;
	return 0;
}

int main(void)
{
	int error_count = 0;
	const struct CMUnitTest hawkbit_server_tests[] = {
	    cmocka_unit_test(test_server_install_update),
	    cmocka_unit_test(test_server_send_deployment_reply),
	    cmocka_unit_test(test_server_send_cancel_reply),
	    cmocka_unit_test(test_server_process_update_artifact),
	    cmocka_unit_test(test_server_set_polling_interval_json),
	    cmocka_unit_test(test_server_has_pending_action)};
	pid = getpid();
	error_count += cmocka_run_group_tests_name(
	    "server_hawkbit", hawkbit_server_tests, server_hawkbit_setup,
	    server_hawkbit_teardown);
	return error_count;
}
