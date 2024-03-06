/*
 * (C) Copyright 2019
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * This is the websocket connection to an external SWUpdate
 * Webserver. It is used to check if a remote update was successful
 * Inspiration for this code comes from libwebsockets
 * "minimal" examples.
 */

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <json-c/json.h>
#include <util.h>
#include <parselib.h>
#include "swuforward_handler.h"

#include <libwebsockets.h>
#include <uriparser/Uri.h>

struct wsconn {
	struct lws *client_wsi;
	struct lws_context *context;
	struct lws_context_creation_info info;
	json_object *json_reply;
	struct curlconn	*conn;	/* Back pointer to main structure */
};

#define TEXTRANGE_TO_STR(f)	(f.first == NULL ? NULL : substring(f.first, 0, f.afterLast - f.first))

static void swupdate_web_answer(struct wsconn *ws, void *in, size_t len)
{
	struct json_tokener *json_tokenizer;
	enum json_tokener_error json_res;
	struct json_object *json_root;

	json_tokenizer = json_tokener_new();

	do {
		json_root = json_tokener_parse_ex(
			json_tokenizer, in, len);
	} while ((json_res = json_tokener_get_error(json_tokenizer)) ==
		json_tokener_continue);
	if (json_res != json_tokener_success) {
		ERROR("Error while parsing answer from %s returned JSON data: %s",
			ws ? ws->conn->url : "", json_tokener_error_desc(json_res));
	} else {
		const char *reply_result = json_get_value(json_root, "type");
		if (reply_result && !strcmp(reply_result, "status")) {
			const char *status = json_get_value(json_root, "status");
			if (!strcmp(status, "SUCCESS"))
				ws->conn->SWUpdateStatus = SUCCESS;
			if (!strcmp(status, "FAILURE"))
				ws->conn->SWUpdateStatus = FAILURE;
			TRACE("Change status on %s : %s", ws->conn->url,
				status);
		}
		if (reply_result && !strcmp(reply_result, "message")) {
			const char *text = json_get_value(json_root, "text");
			TRACE("%s : %s", ws->conn->url, text);
		}
	}
	json_tokener_free(json_tokenizer);
}

static int callback_ws_swupdate(struct lws *wsi, enum lws_callback_reasons reason,
				      void *user, void *in, size_t len)
{
	struct wsconn *ws = (struct wsconn *)user;

	switch (reason) {
	/* because we are protocols[0] ... */
	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		ERROR("WS Client Connection Error to : %s", ws->conn->url);
		ws->client_wsi = NULL;
		ws->conn->connstatus = WS_ERROR;
		break;

	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		TRACE("Connection to %s: established", ws->conn->url);
		ws->conn->connstatus = WS_ESTABLISHED;
		break;

	case LWS_CALLBACK_CLIENT_RECEIVE:
		/*
		 * If it is not connected to SWUpdate's Webserver
		 * call a custom Lua function that should be loaded
		 * in advance
		 */
		if (ws->conn->fnparser && strlen(ws->conn->fnparser)) {
			/*
			 * First convert the incoming data
			 * to a "data" stucture (string)
			 * that is passed to the script.
			 * Raw / Binary data aren't supported
			 */
			char *data = (char *) calloc(1, len + 1);
			if (!data) {
				ERROR("OOM when allocating buffer for Lua Custom Script");
				ws->conn->SWUpdateStatus = FAILURE;
			} else {
				int ret;
				memcpy(data, in, len);
				ret = lua_handler_fn(ws->conn->L, ws->conn->fnparser, data);
				switch (ret) {
				case RUN:
					break;
				case FAILURE:
					ws->conn->SWUpdateStatus = FAILURE;
					break;
				case SUCCESS:
					ws->conn->SWUpdateStatus = SUCCESS;
					break;
				default:
					WARN("Error parsing answer from Webserver, %d", ret);
					ws->conn->SWUpdateStatus = FAILURE;
					break;
				}
				free(data);
			}
		} else {
			swupdate_web_answer(ws, in, len);
		}
		break;

	case LWS_CALLBACK_CLIENT_CLOSED:
		ws->client_wsi = NULL;
		ws->conn->connstatus = WS_CLOSED;
		break;

	default:
		break;
	}

	return lws_callback_http_dummy(wsi, reason, user, in, len);
}

static const struct lws_protocols protocols[] = {
	{
		"swupdate-status-protocol",
		callback_ws_swupdate,
		0,
		0,
	},
	{ NULL, NULL, 0, 0 }
};

int swuforward_ws_connect(struct curlconn *conn) {
	UriParserStateA state;
	UriUriA uri;		/* Parsed URL */
	char *tmp;
	struct wsconn *ws;
	struct lws_context_creation_info *info;
	struct lws_client_connect_info i;
	const char *posturl = conn->url;

	ws = calloc(1, sizeof(*ws));
	if (!ws)
		return -ENOMEM;

	conn->ws = ws;
	ws->conn = conn;

	state.uri = &uri;
	if (uriParseUriA(&state, posturl) != URI_SUCCESS) {
		ERROR("URL seems wrong : %s", posturl);
		return  -EINVAL;
	}

	/*
	 * Initialization
	 */
	info = &ws->info;
	info->options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	info->port = CONTEXT_PORT_NO_LISTEN; /* we do not run any server */
	info->protocols = protocols;

	ws->context = lws_create_context(info);
	if (!ws->context) {
		free(ws);
		ERROR("lws init failed");
		return -EFAULT;
	}

	memset(&i, 0, sizeof i);
	i.context = ws->context;
	i.address = TEXTRANGE_TO_STR(uri.hostText);
	i.path = "/";
	i.host = i.address;
	i.origin = i.address;
	i.protocol = protocols[0].name;
	i.pwsi = &ws->client_wsi;
	i.userdata = ws;
	tmp = TEXTRANGE_TO_STR(uri.portText);
	if (tmp) {
		i.port = strtoul(tmp, NULL, 10);
		free(tmp);
	}

	/*
	 * Check for a valid address before ask for
	 * connection
	 */
	if (!i.address) {
		ERROR("Malformed URL, exiting: %s", posturl);
		return -EINVAL;
	}

	lws_client_connect_via_info(&i);
	free((void *)i.address);

	return 0;
}

int swuforward_ws_getanswer(struct curlconn *conn, int timeout) {
	struct wsconn *ws;
	if (!conn || !conn->ws)
		return -EFAULT;
	ws = conn->ws;
	return lws_service(ws->context, timeout);
}

void swuforward_ws_free(struct curlconn *conn) {
	struct wsconn *ws;

	ws = conn->ws;

	if (ws) {
		lws_context_destroy(ws->context);
		free(ws);
	}
}
