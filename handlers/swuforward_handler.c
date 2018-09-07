/*
 * (C) Copyright 2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

/*
 * This handler allows to create a mesh of devices using SWUpdate
 * as agent. The handler is called if an artifact is a SWU image
 * and sends it to the devices provided in sw-description.
 */

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <swupdate.h>
#include <handler.h>
#include <util.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include "bsdqueue.h"
#include "channel_curl.h"
#include "channel.h"
#include "parselib.h"

/*
 * The Webserver in SWUpdate expets a custom header
 * with the filename
 */
#define CUSTOM_HEADER "X_FILENAME: "
#define MAX_WAIT_MS	30000
#define POST_URL	"/handle_post_request"
#define STATUS_URL	"/getstatus.json"

/*
 * The hzandler checks if a remote update was successful
 * asking for the status. It is supposed that the boards go on
 * until they report a success or failure.
 * Following timeout is introduced in case boards answer, but they do not
 * go out for some reasons from the running state.
 */
#define TIMEOUT_GET_ANSWER_SEC		900	/* 15 minutes */
#define POLLING_TIME_REQ_STATUS		50	/* in mSec */

void swuforward_handler(void);

/*
 * Track each connection
 * The handler maintains a list of connections and sends the SWU
 * to all of them at once.
 */
struct curlconn {
	CURL *curl_handle;	/* CURL handle for posting image */
	const void *buffer;	/* temprary buffer to transfer image */
	unsigned int nbytes;	/* bytes to be transferred per iteration */
	size_t total_bytes;	/* size of SWU image */
	char *url;		/* URL for forwarding */
	bool gotMsg;		/* set if the remote board has sent a new msg */
	RECOVERY_STATUS SWUpdateStatus;	/* final status of update */
	LIST_ENTRY(curlconn) next;
};
LIST_HEAD(listconns, curlconn);

/*
 * global handler data
 *
 */
struct hnd_priv {
	CURLM *cm;		/* libcurl multi handle */
	unsigned int maxwaitms;	/* maximum time in CURL wait */
	size_t size;		/* size of SWU */
	struct listconns conns;	/* list of connections */
};

/*
 * CURL callback when posting data
 * Read from connection buffer and copy to CURL buffer
 */
static size_t curl_read_data(void *buffer, size_t size, size_t nmemb, void *userp)
{
	struct curlconn *conn = (struct curlconn *)userp;
	size_t nbytes;

	if (!nmemb)
		return 0;
	if (!userp) {
		ERROR("Failure IPC stream file descriptor ");
		return -EFAULT;
	}

	if (conn->nbytes > (nmemb * size))
		nbytes = nmemb * size;
	else
		nbytes = conn->nbytes;

	memcpy(buffer, conn->buffer, nbytes);

	conn->nbytes -=  nbytes;

	return nmemb;
}

/*
 * This is the copyimage's callback. When called,
 * there is a buffer to be passed to curl connections
 */
static int swu_forward_data(void *data, const void *buf, unsigned int len)
{
	struct hnd_priv *priv = (struct hnd_priv *)data;
	int ret, still_running = 0;

	struct curlconn *conn;
	LIST_FOREACH(conn, &priv->conns, next) {
		conn->nbytes += len;
		conn->buffer = buf;
	}

	do {
		int ready = 1;

		LIST_FOREACH(conn, &priv->conns, next) {
			if (conn->nbytes > 0) {
				ready = 0;
				break;
			}
		}

		/*
		 * Buffer transferred to all connections,
		 * just returns and wait for next
		 */
		if (ready)
			break;

		int numfds=0;
		ret = curl_multi_wait(priv->cm, NULL, 0, priv->maxwaitms, &numfds);
		if (ret != CURLM_OK) {
			ERROR("curl_multi_wait() returns %d", ret);
			return FAILURE;
		}

		curl_multi_perform(priv->cm, &still_running);
	} while (still_running);

	if (!still_running) {
		LIST_FOREACH(conn, &priv->conns, next) {
			/* check if the buffer was transfered */
			if (conn->nbytes) {
				ERROR("Connection lost, data not transferred");
			}
			conn->total_bytes += len - conn->nbytes;
			if (conn->total_bytes != priv->size) {
				ERROR("Connection lost, SWU not transferred");
				return -EIO;
			}
		}
		ERROR("Connection lost, skipping data");
	}

	return 0;
}

static json_object *parse_reqstatus(json_object *reply, const char **json_path)
{
	json_object *json_data;

	json_data = json_get_path_key(reply, json_path);
	if (json_data == NULL) {
		ERROR("Got malformed JSON: Could not find path");
		DEBUG("Got JSON: %s", json_object_to_json_string(json_data));
	}

	return json_data;
}
/*
 * Send a GET to retrieve all traces from the connected board
 */
static int get_answer(struct curlconn *conn, RECOVERY_STATUS *result, bool ignore)
{
	channel_data_t channel_cfg = {
		.debug = false,
		.retries = 0,
		.retry_sleep = 0,
		.usessl = false};
	channel_op_res_t response;
	channel_t *channel = channel_new();
	json_object *json_data;
	int status;

	/*
	 * Open a curl channel, do not connect yet
	 */
	if (channel->open(channel, &channel_cfg) != CHANNEL_OK) {
		return -EIO;
	}

	if (asprintf(&channel_cfg.url, "%s%s",
			 conn->url, STATUS_URL) < 0) {
		ERROR("Out of memory.");
		return -ENOMEM; 
	}

	/* Retrieve last message */
	response = channel->get(channel, (void *)&channel_cfg);

	if (response != CHANNEL_OK) {
		channel->close(channel);
		free(channel);
		free(channel_cfg.url);
		return -1;
	}

	/* Retrieve all fields */
	status = -EBADMSG;
	if (!(json_data = parse_reqstatus(channel_cfg.json_reply,
					(const char *[]){"Status", NULL})))
		goto cleanup;
	status = json_object_get_int(json_data);

	if (!(json_data = parse_reqstatus(channel_cfg.json_reply,
					(const char *[]){"Msg", NULL})))
		goto cleanup;
	const char *msg = json_object_get_string(json_data);

	if (!(json_data = parse_reqstatus(channel_cfg.json_reply,
					(const char *[]){"LastResult", NULL})))
		goto cleanup;
	int lastResult = json_object_get_int(json_data);

	if (strlen(msg) > 0)
		conn->gotMsg = (strlen(msg) > 0) ? true : false;

	if (!ignore) {
		if (strlen(msg)) {
			TRACE("Update to %s :  %s", conn->url, msg);
		}
		if (status == IDLE) {
			TRACE("Update to %s : %s", conn->url, 
				(lastResult == SUCCESS) ? "SUCCESS !" : "FAILURE");
		}
	}

	*result = lastResult;

cleanup:
	free(channel_cfg.url);
	channel->close(channel);
	free(channel);

	return status;
}

static int retrieve_msgs(struct hnd_priv *priv, bool ignore)
{
	struct curlconn *conn;
	int ret;
	int result = 0;

	LIST_FOREACH(conn, &priv->conns, next) {
		int count = 0;
		do {
			ret = get_answer(conn, &conn->SWUpdateStatus, ignore);
			if (!conn->gotMsg) {
				usleep(POLLING_TIME_REQ_STATUS * 1000);
				count++;
			} else
				count = 0;
			if (count > ((TIMEOUT_GET_ANSWER_SEC * 1000) /
					POLLING_TIME_REQ_STATUS)) {
				ret = -ETIMEDOUT;
			}
		} while (ret > 0);
		if (ret != 0 || (conn->SWUpdateStatus != SUCCESS)) {
			ERROR("Update to %s was NOT successful !", conn->url);
			result = -1;
		}
	}

	return result;
}

static int install_remote_swu(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	struct hnd_priv priv;
	struct curlconn *conn;
	int ret, still_running = 0;
	struct dict_list_elem *url;
	struct curl_slist *headerlist;
	CURLMsg *msg = NULL;
	struct dict_list *urls;

	/*
	 * A single SWU can contains encrypted artifacts,
	 * but the SWU itself canot be encrypted.
	 * Raise an error if the encrypted attribute is set
	 */

	if (img->is_encrypted) {
		ERROR("SWU to be forwarded cannot be encrypted");
		return -EINVAL;
	}

	/*
	 * Check if there is a list of URLs where to forward
	 * the SWU
	 */
	urls = dict_get_list(&img->properties, "url");
	if (!urls) {
		ERROR("SWU to be forwarded, but not remote URLs found ");
		return -EINVAL;
	}

	/* Reset list of connections */
	LIST_INIT(&priv.conns);

	/* initialize CURL */
	ret = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (ret != CURLE_OK) {
		ret = FAILURE;
		goto handler_exit;
	}
	priv.cm = curl_multi_init();
	priv.maxwaitms = MAX_WAIT_MS;
	priv.size = img->size;

	LIST_FOREACH(url, urls, next) {
		char curlheader[SWUPDATE_GENERAL_STRING_SIZE + strlen(CUSTOM_HEADER)];

		conn = (struct curlconn *)calloc(1, sizeof(struct curlconn));
		if (!conn) {
			ERROR("FAULT: no memory");
			ret = -ENOMEM;
			goto handler_exit;
		}

		headerlist = NULL;

		conn->curl_handle = curl_easy_init();
		conn->url = url->value;

		if (!conn->curl_handle) {
			/* something very bad, it should never happen */
			ERROR("FAULT: no handle from libcurl");
			return FAILURE;
		}

		snprintf(curlheader, sizeof(curlheader), "%s%s", CUSTOM_HEADER, img->fname);
		headerlist = curl_slist_append(headerlist, curlheader);

		if ((curl_easy_setopt(conn->curl_handle, CURLOPT_POST, 1L) != CURLE_OK) ||
		    (curl_easy_setopt(conn->curl_handle, CURLOPT_READFUNCTION,
				      curl_read_data) != CURLE_OK) ||
		    (curl_easy_setopt(conn->curl_handle, CURLOPT_READDATA,
				      conn) !=CURLE_OK) ||
	    	    (curl_easy_setopt(conn->curl_handle, CURLOPT_USERAGENT,
			      "libcurl-agent/1.0") != CURLE_OK) ||
		    (curl_easy_setopt(conn->curl_handle, CURLOPT_POSTFIELDSIZE,
				      img->size)!=CURLE_OK) || 
		    (curl_easy_setopt(conn->curl_handle, CURLOPT_HTTPHEADER,
				      headerlist) != CURLE_OK)) { 
			ERROR("curl set_option was not successful");
			ret = FAILURE;
			goto handler_exit;
		}

		/* get verbose debug output please */ 
		curl_easy_setopt(conn->curl_handle, CURLOPT_VERBOSE, 1L);

		char *posturl = NULL;
		posturl = (char *)malloc(strlen(conn->url) + strlen(POST_URL) + 1);
		sprintf(posturl, "%s%s", conn->url, POST_URL); 

		/* Set URL */
		if (curl_easy_setopt(conn->curl_handle, CURLOPT_URL, posturl) != CURLE_OK) {
			ERROR("Cannot set URL in libcurl");
			free(posturl);
			ret = FAILURE;
			goto handler_exit;
		}
		free(posturl);
		curl_multi_add_handle(priv.cm, conn->curl_handle);
		LIST_INSERT_HEAD(&priv.conns, conn, next);
	}

	retrieve_msgs(&priv, true);

	curl_multi_perform(priv.cm, &still_running);

	ret = copyimage(&priv, img, swu_forward_data);

	if (ret) {
		ERROR("Transferring SWU image was not successful");
		goto handler_exit;
	}

	/*
	 * Now checks if transfer was successful
	 */
	int msgs_left = 0;
	while ((msg = curl_multi_info_read(priv.cm, &msgs_left))) {
		CURL *eh = NULL;
		int http_status_code=0;
		if (msg->msg != CURLMSG_DONE) {
			ERROR("curl_multi_info_read(), CURLMsg=%d", msg->msg);
			ret = FAILURE;
			break;
		}
		LIST_FOREACH(conn, &priv.conns, next) {
			if (conn->curl_handle == msg->easy_handle) {
				eh = conn->curl_handle;
				break;
			}
		}

		if (!eh) {
			ERROR("curl handle not found in connections");
			ret = FAILURE;
			break;
		}

		curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &http_status_code);

		if (http_status_code != 200) {
			ERROR("Sending %s to %s failed with %d",
				img->fname, conn->url, http_status_code);
			ret = FAILURE;
			break;
		}
	}

	/*
	 * Now check if remote updates were successful
	 */
	if (!ret) {
		ret = retrieve_msgs(&priv, false);
	}

handler_exit:
	LIST_FOREACH(conn, &priv.conns, next) {
		LIST_REMOVE(conn, next);
		curl_multi_remove_handle(priv.cm, conn->curl_handle);
		curl_easy_cleanup(conn->curl_handle);
		free(conn);
	}

	curl_multi_cleanup(priv.cm);

	return ret;
}

__attribute__((constructor))
void swuforward_handler(void)
{
	register_handler("swuforward", install_remote_swu,
				IMAGE_HANDLER, NULL);
}
