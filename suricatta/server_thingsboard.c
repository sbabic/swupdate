/*
 * (C) Copyright 2023-2026
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * Suricatta server for the OTA updates of ThingsBoard, through its
 * HTTP device API. The device is authenticated with its access token.
 *
 * ThingsBoard sets the shared attributes <p>_title and <p>_version
 * (p = fw for firmware, sw for software packages) when a package is
 * assigned to the device or to its profile. When they differ from the
 * package running on the device, the package is installed, and its
 * state is reported as telemetry: <p>_state, <p>_error and, when the
 * update is done, current_<p>_title and current_<p>_version.
 *
 * The package running on the device and the one being installed are
 * kept in SWUpdate's variables, so that the result of an update can be
 * reported after the reboot.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <json-c/json.h>
#include <generated/autoconf.h>
#include <util.h>
#include <network_ipc.h>
#include <swupdate_status.h>
#include <swupdate_vars.h>
#include "suricatta/suricatta.h"
#include "suricatta/server.h"
#include "server_utils.h"
#include "parselib.h"
#include "channel.h"
#include "channel_curl.h"
#include "state.h"
#include "swupdate_settings.h"
#include "server_thingsboard.h"

/*
 * Kinds of package stored in the variables thingsboard_<p>_[<kind>]title
 * and thingsboard_<p>_[<kind>]version
 */
#define PKG_CURRENT	""
#define PKG_PENDING	"pending_"
#define PKG_FAILED	"failed_"

/* bits to check that all mandatory settings are given */
#define URL_BIT		1
#define TOKEN_BIT	2
#define ALL_MANDATORY_SET	(URL_BIT | TOKEN_BIT)

extern channel_op_res_t channel_curl_init(void);

static struct option long_options[] = {
    {"url", required_argument, NULL, 'u'},
    {"targettoken", required_argument, NULL, 'k'},
    {"package", required_argument, NULL, 'P'},
    {"title", required_argument, NULL, 'T'},
    {"version", required_argument, NULL, 'V'},
    {"confirm", required_argument, NULL, 'c'},
    {"polldelay", required_argument, NULL, 'p'},
    {"nocheckcert", no_argument, NULL, 'x'},
    {"retry", required_argument, NULL, 'r'},
    {"retrywait", required_argument, NULL, 'w'},
    {"proxy", optional_argument, NULL, 'y'},
    {"interface", required_argument, NULL, 'f'},
    {"cache", required_argument, NULL, '2'},
    {"connection-timeout", required_argument, NULL, 's'},
    {"max-download-speed", required_argument, NULL, 'n'},
    {"server", required_argument, NULL, 'S'},
    {NULL, 0, NULL, 0}};

static unsigned int mandatory_argument_count = 0;

static server_thingsboard_t server_thingsboard = {
	.polling_interval = CHANNEL_DEFAULT_POLLING_INTERVAL,
	.prefix = "fw",
	.update_state = STATE_NOT_AVAILABLE,
};

static channel_data_t channel_data_defaults = {.debug = false,
					       .source = SOURCE_SURICATTA,
					       .retries = CHANNEL_DEFAULT_RESUME_TRIES,
					       .retry_sleep =
						   CHANNEL_DEFAULT_RESUME_DELAY,
#ifdef CONFIG_SURICATTA_SSL
					       .usessl = true,
#endif
					       .format = CHANNEL_PARSE_NONE,
					       .nocheckanswer = false,
					       .nofollow = false,
					       .strictssl = true,
					       .noipc = false};

/* first error reported by the installer, sent as <p>_error */
static char install_error[TB_STRING_SIZE];

static int server_status_callback(ipc_message *msg)
{
	if (msg->data.status.current == FAILURE && strlen(msg->data.status.desc) &&
	    !strlen(install_error))
		strlcpy(install_error, msg->data.status.desc, sizeof(install_error));

	return 0;
}

static char *url_escape(const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	char *out = malloc(strlen(s) * 3 + 1);
	char *p = out;

	if (!out)
		return NULL;
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
			*p++ = (char)c;
		} else {
			*p++ = '%';
			*p++ = hex[c >> 4];
			*p++ = hex[c & 0x0f];
		}
	}
	*p = '\0';

	return out;
}

/*
 * URL of a resource of the device API, the access token is part of it:
 * do not log it.
 */
static char *api_url(const char *resource, const char *query)
{
	char *token = url_escape(server_thingsboard.token);
	char *url;

	if (!token)
		return NULL;
	if (asprintf(&url, "%s/api/v1/%s/%s%s%s", server_thingsboard.url, token,
		     resource, query ? "?" : "", query ? query : "") == ENOMEM_ASPRINTF)
		url = NULL;
	free(token);

	return url;
}

/* name of an OTA key for the package type, e.g. fw_title, current_fw_version */
static const char *ota_key(char *buf, size_t size, const char *before, const char *name)
{
	snprintf(buf, size, "%s%s_%s", before, server_thingsboard.prefix, name);

	return buf;
}

static void json_add_string(json_object *obj, const char *before, const char *name,
			    const char *value)
{
	char key[64];

	json_object_object_add(obj, ota_key(key, sizeof(key), before, name),
			       json_object_new_string(value));
}

static server_op_res_t send_telemetry(json_object *values)
{
	channel_data_t channel_data = channel_data_defaults;
	channel_t *channel = server_thingsboard.channel;
	server_op_res_t result;
	char *url = api_url("telemetry", NULL);

	if (!url) {
		json_object_put(values);
		return SERVER_EERR;
	}

	channel_data.url = url;
	channel_data.request_body = (char *)json_object_to_json_string(values);
	channel_data.method = CHANNEL_POST;
	channel_data.content_type = "application/json";
	DEBUG("Report to ThingsBoard: %s", channel_data.request_body);
	result = map_channel_retcode(channel->put(channel, (void *)&channel_data));
	if (result != SERVER_OK)
		ERROR("Cannot report to ThingsBoard: %s", channel_data.request_body);

	free(url);
	json_object_put(values);

	return result;
}

/*
 * Report the state of the update, with the package running on the device
 * if current is set
 */
static server_op_res_t report_state(const char *state, const char *error, bool current)
{
	json_object *values = json_object_new_object();

	if (!values)
		return SERVER_EERR;
	json_add_string(values, "", "state", state);
	json_add_string(values, "", "error", error ? error : "");
	if (current) {
		json_add_string(values, "current_", "title", server_thingsboard.title);
		json_add_string(values, "current_", "version", server_thingsboard.version);
	}
	if (send_telemetry(values) != SERVER_OK)
		return SERVER_EERR;
	if (current)
		server_thingsboard.announced = true;

	return SERVER_OK;
}

/* report the package running on the device, without a state */
static server_op_res_t report_current(void)
{
	json_object *values = json_object_new_object();

	if (!values)
		return SERVER_EERR;
	json_add_string(values, "current_", "title", server_thingsboard.title);
	json_add_string(values, "current_", "version", server_thingsboard.version);

	return send_telemetry(values);
}

static const char *var_name(char *buf, size_t size, const char *kind, const char *name)
{
	snprintf(buf, size, "thingsboard_%s_%s%s", server_thingsboard.prefix, kind, name);

	return buf;
}

/* store a package in the variables, or drop it if title is NULL */
static bool store_package(const char *kind, const char *title, const char *version)
{
	char name[64];

	return !swupdate_vars_set(var_name(name, sizeof(name), kind, "title"), title, NULL) &&
	       !swupdate_vars_set(var_name(name, sizeof(name), kind, "version"), version, NULL);
}

/* read a package from the variables: false if it is not set */
static bool load_package(const char *kind, char *title, char *version)
{
	char name[64];
	char *t = swupdate_vars_get(var_name(name, sizeof(name), kind, "title"), NULL);
	char *v = swupdate_vars_get(var_name(name, sizeof(name), kind, "version"), NULL);
	bool found = t && v && strlen(t) && strlen(v);

	if (found) {
		strlcpy(title, t, TB_STRING_SIZE);
		strlcpy(version, v, TB_STRING_SIZE);
	}
	free(t);
	free(v);

	return found;
}

static void set_current(const char *title, const char *version)
{
	strlcpy(server_thingsboard.title, title, sizeof(server_thingsboard.title));
	strlcpy(server_thingsboard.version, version, sizeof(server_thingsboard.version));
	if (!store_package(PKG_CURRENT, title, version))
		ERROR("Cannot store the version of the installed package: "
		      "it is lost after a restart");
}

static bool set_pending(const char *title, const char *version)
{
	return store_package(PKG_PENDING, title, version);
}

static void clear_pending(void)
{
	(void)store_package(PKG_PENDING, NULL, NULL);
}

/* the package is not installed again, also after a restart */
static void set_failed(const char *title, const char *version)
{
	strlcpy(server_thingsboard.failed_title, title,
		sizeof(server_thingsboard.failed_title));
	strlcpy(server_thingsboard.failed_version, version,
		sizeof(server_thingsboard.failed_version));
	if (!store_package(PKG_FAILED, title, version))
		WARN("Cannot store the failed package");
}

/*
 * Report the result of the update installed before the reboot. With the
 * update state INSTALLED the new software is not tested yet, the result
 * is reported later: return SERVER_EAGAIN to be called again.
 */
static server_op_res_t server_handle_initial_state(void)
{
	update_state_t state = server_thingsboard.update_state;
	char title[TB_STRING_SIZE], version[TB_STRING_SIZE];
	server_op_res_t result;
	bool pending;

	if (state == STATE_NOT_AVAILABLE)
		state = get_state();

	switch (state) {
	case STATE_INSTALLED:
		DEBUG("Update installed, testing pending.");
		return SERVER_EAGAIN;
	case STATE_TESTING:
	case STATE_FAILED:
		break;
	default:
		return SERVER_OK;
	}

	pending = load_package(PKG_PENDING, title, version);
	if (state == STATE_TESTING) {
		if (pending)
			set_current(title, version);
		else
			WARN("Update installed, but the installed package is unknown");
		result = report_state("UPDATED", NULL, true);
	} else {
		if (pending)
			set_failed(title, version);
		result = report_state("FAILED", "Update failed after the reboot", false);
	}
	if (result != SERVER_OK)
		return result;

	clear_pending();
	/*
	 * (Re-)set the state to OK instead of dropping it, it may be
	 * required by the bootloader logic
	 */
	if (save_state(STATE_OK) != SERVER_OK)
		WARN("Cannot reset the update state");
	server_thingsboard.update_state = STATE_NOT_AVAILABLE;

	return SERVER_OK;
}

static bool json_get_shared(json_object *root, const char *name, const char **value)
{
	char key[64];
	json_object *obj = json_get_path_key(root,
		(const char *[]){"shared", ota_key(key, sizeof(key), "", name), NULL});

	*value = NULL;
	if (!obj)
		return true;
	if (!json_object_is_type(obj, json_type_string)) {
		ERROR("ThingsBoard attribute %s is not a string", key);
		return false;
	}
	*value = json_object_get_string(obj);
	if (strlen(*value) >= TB_STRING_SIZE) {
		ERROR("ThingsBoard attribute %s is too long", key);
		return false;
	}

	return true;
}

static server_op_res_t server_get_deployment_info(void)
{
	channel_data_t channel_data = channel_data_defaults;
	channel_t *channel = server_thingsboard.channel;
	const char *prefix = server_thingsboard.prefix;
	const char *title, *version, *url;
	server_op_res_t result;
	char query[128];

	snprintf(query, sizeof(query), "sharedKeys=%s_title,%s_version,%s_url",
		 prefix, prefix, prefix);
	channel_data.url = api_url("attributes", query);
	if (!channel_data.url)
		return SERVER_EERR;
	channel_data.format = CHANNEL_PARSE_JSON;

	result = map_channel_retcode(channel->get(channel, (void *)&channel_data));
	free(channel_data.url);
	if (result != SERVER_OK)
		goto out;

	if (!channel_data.json_reply ||
	    !json_get_shared(channel_data.json_reply, "title", &title) ||
	    !json_get_shared(channel_data.json_reply, "version", &version) ||
	    !json_get_shared(channel_data.json_reply, "url", &url)) {
		result = SERVER_EBADMSG;
		goto out;
	}

	if (!title || !version) {
		DEBUG("No package assigned to the device");
		result = SERVER_NO_UPDATE_AVAILABLE;
		goto out;
	}
	if (!strcmp(title, server_thingsboard.title) &&
	    !strcmp(version, server_thingsboard.version)) {
		DEBUG("Package %s %s already installed", title, version);
		result = SERVER_NO_UPDATE_AVAILABLE;
		goto out;
	}
	if (!strcmp(title, server_thingsboard.failed_title) &&
	    !strcmp(version, server_thingsboard.failed_version)) {
		DEBUG("Package %s %s failed, it is not installed again", title, version);
		result = SERVER_NO_UPDATE_AVAILABLE;
		goto out;
	}

	strlcpy(server_thingsboard.target_title, title,
		sizeof(server_thingsboard.target_title));
	strlcpy(server_thingsboard.target_version, version,
		sizeof(server_thingsboard.target_version));
	free(server_thingsboard.target_url);
	server_thingsboard.target_url = (url && strlen(url)) ? strdup(url) : NULL;
	INFO("New package on ThingsBoard: %s %s", title, version);
	result = SERVER_UPDATE_AVAILABLE;

out:
	if (channel_data.json_reply)
		json_object_put(channel_data.json_reply);

	return result;
}

static server_op_res_t server_has_pending_action(int *action_id)
{
	*action_id = 0;

	if (server_thingsboard.initial_report) {
		if (server_handle_initial_state() != SERVER_OK)
			return SERVER_OK;
		server_thingsboard.initial_report = false;
	}

	if (get_state() == STATE_INSTALLED) {
		WARN("An already installed update is pending testing.");
		return SERVER_NO_UPDATE_AVAILABLE;
	}

	if (!server_thingsboard.announced && strlen(server_thingsboard.title))
		server_thingsboard.announced = report_current() == SERVER_OK;

	return server_get_deployment_info();
}

static char *download_url(void)
{
	char *title, *version, *query = NULL, *url = NULL;

	/* package stored outside ThingsBoard */
	if (server_thingsboard.target_url)
		return strdup(server_thingsboard.target_url);

	title = url_escape(server_thingsboard.target_title);
	version = url_escape(server_thingsboard.target_version);
	if (title && version &&
	    asprintf(&query, "title=%s&version=%s", title, version) != ENOMEM_ASPRINTF) {
		url = api_url(!strcmp(server_thingsboard.prefix, "fw") ?
			      "firmware" : "software", query);
		free(query);
	}
	free(title);
	free(version);

	return url;
}

static bool run_postupdate(void)
{
	ipc_message msg;

	INFO("Update successful, executing post-update actions.");
	memset(&msg, 0, sizeof(msg));
	if (ipc_postupdate(&msg) != 0 || msg.type != ACK) {
		ERROR("Post-update actions failed");
		return false;
	}
	DEBUG("%s", msg.data.msg);

	return true;
}

static server_op_res_t server_install_update(void)
{
	channel_data_t channel_data = channel_data_defaults;
	channel_t *channel = server_thingsboard.channel;
	const char *title = server_thingsboard.target_title;
	const char *version = server_thingsboard.target_version;
	const char *error = NULL;
	server_op_res_t result;

	channel_data.url = download_url();
	if (!channel_data.url)
		return SERVER_EERR;
	if (server_thingsboard.cached_file)
		channel_data.cached_file = server_thingsboard.cached_file;

	install_error[0] = '\0';
	/*
	 * Needed after the reboot to report the result, and to know the
	 * installed package: without it, the package would be installed
	 * again after each reboot
	 */
	if (!set_pending(title, version)) {
		ERROR("Cannot store the package being installed, "
		      "is namespace-vars set ?");
		result = SERVER_EERR;
		error = "Cannot store the package on the device";
		goto failed;
	}
	(void)report_state("DOWNLOADING", NULL, false);

	result = map_channel_retcode(channel->get_file(channel, (void *)&channel_data));
	if (result != SERVER_OK) {
		/* called to collect the errors */
		ipc_wait_for_complete(server_status_callback);
		error = "Download failed";
		goto failed;
	}
	if (ipc_wait_for_complete(server_status_callback) == FAILURE) {
		result = SERVER_EERR;
		error = "Installation failed";
		goto failed;
	}
	(void)report_state("DOWNLOADED", NULL, false);

	if (get_state() == STATE_INSTALLED) {
		/*
		 * The update is done after the reboot, its result is
		 * reported then by server_handle_initial_state()
		 */
		(void)report_state("UPDATING", NULL, false);
		server_thingsboard.initial_report = true;
		if (!run_postupdate())
			result = SERVER_EERR;
		goto cleanup;
	}

	/* without update state the update is done */
	if (!run_postupdate()) {
		result = SERVER_EERR;
		error = "Post-update actions failed";
		goto failed;
	}
	set_current(title, version);
	clear_pending();
	(void)report_state("UPDATED", NULL, true);
	goto cleanup;

failed:
	ERROR("Update to %s %s failed", title, version);
	(void)report_state("FAILED", strlen(install_error) ? install_error : error, false);
	set_failed(title, version);
	clear_pending();
	/* reported: do not report it again after a restart */
	if (get_state() == STATE_FAILED && save_state(STATE_OK) != SERVER_OK)
		WARN("Cannot reset the update state");

cleanup:
	free(channel_data.url);
	if (server_thingsboard.cached_file) {
		free(server_thingsboard.cached_file);
		server_thingsboard.cached_file = NULL;
	}

	return result;
}

static server_op_res_t server_send_target_data(void)
{
	return SERVER_OK;
}

static unsigned int server_get_polling_interval(void)
{
	return server_thingsboard.polling_interval;
}

static void server_print_help(void)
{
	fprintf(
	    stdout,
	    "\t  -u, --url         * URL of the ThingsBoard server, "
	    "e.g., http://localhost:8080\n"
	    "\t  -k, --targettoken * Access token of the device.\n"
	    "\t  --package           Package type: firmware (default) or software.\n"
	    "\t  --title             Title of the package running on the device.\n"
	    "\t  --version           Version of the package running on the device.\n"
	    "\t  -c, --confirm       Confirm update status to server: 1=AGAIN, "
	    "2=SUCCESS, 3=FAILED\n"
	    "\t  -p, --polldelay     Delay in seconds between two server "
	    "poll operations (default: %ds).\n"
	    "\t  -x, --nocheckcert   Do not abort on flawed server "
	    "certificates.\n"
	    "\t  -r, --retry         Resume and retry interrupted downloads "
	    "(default: %d tries).\n"
	    "\t  -w, --retrywait     Time to wait prior to retry and "
	    "resume a download (default: %ds).\n"
	    "\t  -y, --proxy         Use proxy. Either give proxy URL, else "
	    "{http,all}_proxy env is tried.\n"
	    "\t  -f, --interface     Set the network interface to connect "
	    "to the server.\n"
	    "\t  -2, --cache         Read the SWU from this file before "
	    "downloading it.\n"
	    "\t  -s, --connection-timeout Set the server connection timeout "
	    "(default: 300s).\n"
	    "\t  -n, --max-download-speed <limit> Set download speed limit. "
	    "Example: -n 100k; -n 1M; -n 100; -n 1G\n",
	    CHANNEL_DEFAULT_POLLING_INTERVAL, CHANNEL_DEFAULT_RESUME_TRIES,
	    CHANNEL_DEFAULT_RESUME_DELAY);
}

static bool set_package(const char *package)
{
	if (!strcmp(package, "firmware"))
		server_thingsboard.prefix = "fw";
	else if (!strcmp(package, "software"))
		server_thingsboard.prefix = "sw";
	else {
		ERROR("Unknown package type %s: firmware or software", package);
		return false;
	}

	return true;
}

static int server_thingsboard_settings(void *elem, void __attribute__ ((__unused__)) *data)
{
	char tmp[TB_STRING_SIZE];

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "url", tmp);
	if (strlen(tmp)) {
		SETSTRING(server_thingsboard.url, tmp);
		mandatory_argument_count |= URL_BIT;
	}

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "targettoken", tmp);
	if (strlen(tmp)) {
		SETSTRING(server_thingsboard.token, tmp);
		mandatory_argument_count |= TOKEN_BIT;
	}

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "package", tmp);
	if (strlen(tmp) && !set_package(tmp))
		return -EINVAL;

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "title", tmp);
	if (strlen(tmp))
		strlcpy(server_thingsboard.title, tmp, sizeof(server_thingsboard.title));

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "version", tmp);
	if (strlen(tmp))
		strlcpy(server_thingsboard.version, tmp, sizeof(server_thingsboard.version));

	GET_FIELD_INT(LIBCFG_PARSER, elem, "polldelay",
		(int *)&server_thingsboard.polling_interval);

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "connection-timeout", tmp);
	if (strlen(tmp))
		channel_data_defaults.connection_timeout =
			(unsigned int)strtoul(tmp, NULL, 10);

	channel_settings(elem, &channel_data_defaults);

	return 0;
}

static server_op_res_t server_start(const char *fname, int argc, char *argv[])
{
	int choice = 0;
	size_t len;

	mandatory_argument_count = 0;

	if (fname) {
		swupdate_cfg_handle handle;
		int ret = 0;

		swupdate_cfg_init(&handle);
		if (swupdate_cfg_read_file(&handle, fname) == 0)
			ret = read_module_settings(&handle, "thingsboard",
						   server_thingsboard_settings, NULL);
		swupdate_cfg_destroy(&handle);
		if (ret)
			return SERVER_EINIT;
	}

	if (loglevel >= DEBUGLEVEL)
		channel_data_defaults.debug = true;

	/* reset to optind=1 to parse suricatta's argument vector */
	optind = 1;
	opterr = 0;
	while ((choice = getopt_long(argc, argv, "u:k:P:T:V:c:p:xr:w:y::f:2:s:n:S:",
				     long_options, NULL)) != -1) {
		switch (choice) {
		case 'u':
			SETSTRING(server_thingsboard.url, optarg);
			mandatory_argument_count |= URL_BIT;
			break;
		case 'k':
			SETSTRING(server_thingsboard.token, optarg);
			mandatory_argument_count |= TOKEN_BIT;
			break;
		case 'P':
			if (!set_package(optarg))
				return SERVER_EINIT;
			break;
		case 'T':
			strlcpy(server_thingsboard.title, optarg,
				sizeof(server_thingsboard.title));
			break;
		case 'V':
			strlcpy(server_thingsboard.version, optarg,
				sizeof(server_thingsboard.version));
			break;
		case 'c':
			/*
			 * Without persistent update state, the state to be
			 * reported is given on the command line
			 */
			server_thingsboard.update_state = (update_state_t)*optarg;
			switch (server_thingsboard.update_state) {
			case STATE_OK:
			case STATE_INSTALLED:
			case STATE_TESTING:
			case STATE_FAILED:
				break;
			default:
				ERROR("Invalid update status given.");
				suricatta_print_help();
				return SERVER_EINIT;
			}
			break;
		case 'p':
			server_thingsboard.polling_interval =
			    (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'x':
			channel_data_defaults.strictssl = false;
			break;
		case 'r':
			channel_data_defaults.retries =
			    (unsigned char)strtoul(optarg, NULL, 10);
			break;
		case 'w':
			channel_data_defaults.retry_sleep =
			    (unsigned int)strtoul(optarg, NULL, 10);
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
					ERROR("Should use proxy but no proxy "
					      "environment variables nor proxy "
					      "URL set.");
					return SERVER_EINIT;
				}
				channel_data_defaults.proxy = USE_PROXY_ENV;
			}
			break;
		case 'f':
			SETSTRING(channel_data_defaults.iface, optarg);
			break;
		case '2':
			SETSTRING(server_thingsboard.cached_file, optarg);
			break;
		case 's':
			channel_data_defaults.connection_timeout =
				(unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'n':
			channel_data_defaults.max_download_speed =
				(unsigned int)ustrtoull(optarg, NULL, 10);
			if (errno) {
				ERROR("max-download-speed %s: ustrtoull failed",
				      optarg);
				return SERVER_EINIT;
			}
			break;
		/* Ignore the --server option which is already parsed by the caller. */
		case 'S':
			break;
		case '?':
		/* Ignore not recognized options, they can be already parsed by the caller */
		default:
			break;
		}
	}

	if (mandatory_argument_count != ALL_MANDATORY_SET) {
		ERROR("Mandatory arguments missing!");
		suricatta_print_help();
		return SERVER_EINIT;
	}
	if (optind < argc) {
		ERROR("Unused argument(s) given to suricatta (-u), see --help.");
		suricatta_print_help();
		return SERVER_EINIT;
	}

	/* the API path is appended to the URL */
	len = strlen(server_thingsboard.url);
	while (len > 0 && server_thingsboard.url[len - 1] == '/')
		server_thingsboard.url[--len] = '\0';

	/* the package installed by suricatta wins over the setup */
	(void)load_package(PKG_CURRENT, server_thingsboard.title, server_thingsboard.version);
	(void)load_package(PKG_FAILED, server_thingsboard.failed_title,
			   server_thingsboard.failed_version);
	if (strlen(server_thingsboard.title))
		INFO("ThingsBoard: running package %s %s",
		     server_thingsboard.title, server_thingsboard.version);

	if (channel_curl_init() != CHANNEL_OK)
		return SERVER_EINIT;

	server_thingsboard.channel = channel_new();
	if (!server_thingsboard.channel)
		return SERVER_EINIT;

	if (server_thingsboard.channel->open(server_thingsboard.channel,
					     &channel_data_defaults) != CHANNEL_OK) {
		(void)server_thingsboard.channel->close(server_thingsboard.channel);
		free(server_thingsboard.channel);
		return SERVER_EINIT;
	}

	/* the result of an update before the reboot is reported at the first poll */
	server_thingsboard.initial_report = true;

	return SERVER_OK;
}

static server_op_res_t server_stop(void)
{
	(void)server_thingsboard.channel->close(server_thingsboard.channel);
	free(server_thingsboard.channel);
	server_thingsboard.channel = NULL;
	free(server_thingsboard.target_url);
	server_thingsboard.target_url = NULL;

	return SERVER_OK;
}

static server_op_res_t server_ipc(ipc_message __attribute__ ((__unused__)) *msg)
{
	return SERVER_OK;
}

static server_t server = {
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
static void register_server_thingsboard(void)
{
	register_server("thingsboard", &server);
}
