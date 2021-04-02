/*
 * (C) Copyright 2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * This handler allows to create a mesh of devices using SWUpdate
 * as agent. The handler is called if an artifact is a SWU image
 * and sends it to the devices provided in sw-description.
 *
 * To provide zero-copy and support for installing on multiple
 * remote devices, the multi interface in libcurl is used.
 *
 * This handler spawns a task to provide callback with libcurl.
 * The main task has an own callback for copyimage(), and
 * writes into cretated FIFOs (one for each remote device)
 * because the connections to devices is asynchrounous.
 *
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
#include <pthread.h>
#include <util.h>
#include <json-c/json.h>
#include "parselib.h"
#include "swuforward_handler.h"

void swuforward_handler(void);

/*
 * global handler data
 *
 */
struct hnd_priv {
	unsigned int maxwaitms;	/* maximum time in CURL wait */
	struct listconns conns;	/* list of connections */
};

/*
 * CURL callback when posting data
 * Read from connection buffer and copy to CURL buffer
 */
static size_t curl_read_data(char *buffer, size_t size, size_t nmemb, void *userp)
{
	struct curlconn *conn = (struct curlconn *)userp;
	ssize_t nbytes;

	if (!nmemb)
		return 0;
	if (!userp) {
		ERROR("Failure IPC stream file descriptor ");
		return CURL_READFUNC_ABORT;
	}

	if (nmemb * size > conn->total_bytes)
		nbytes =  conn->total_bytes;
	else
		nbytes = nmemb * size;

	nbytes = read(conn->fifo[0], buffer, nbytes);
	if (nbytes == -1 && errno == EAGAIN) {
		TRACE("No data, try again");
		nbytes = 0;
	}

	if (nbytes < 0) {
		ERROR("Cannot read from FIFO");
		return CURL_READFUNC_ABORT;
	}

	nmemb = nbytes / size;

	conn->total_bytes -=  nbytes;

	return nmemb;
}

/*
 * This is the copyimage's callback. When called,
 * there is a buffer to be passed to curl connections
 */
static int swu_forward_data(void *data, const void *buf, unsigned int len)
{
	struct hnd_priv *priv = (struct hnd_priv *)data;
	ssize_t written;
	struct curlconn *conn;
	int index = 0;

	/*
	 * Iterate all connection and copy the incoming buffer
	 * to the corresponding FIFO.
	 * Each connection has own FIFO to transfer the data
	 * to the curl thread
	 */
	LIST_FOREACH(conn, &priv->conns, next) {
		unsigned int nbytes = len;
		const void *tmp = buf;

		while (nbytes) {
			written = write(conn->fifo[1], buf, len);

			if (written < 0) {
				ERROR ("Cannot write to fifo %d", index);
				return -EFAULT;
			}
			nbytes -= written;
			tmp += written;
		}
	}

	return 0;
}

/*
 * Internal thread to transfer the SWUs to
 * the other devices.
 * The thread reads the per-connection FIFO and handles
 * the curl multi interface.
 */
static void *curl_transfer_thread(void *p)
{
	struct curlconn *conn = (struct curlconn *)p;
	const char no_100_header[] = "Expect:";
	struct curl_slist *headerlist;
	curl_mimepart *field = NULL;
	headerlist = NULL;


	/*
	 * This is just to run the scheduler and let the main
	 * thread to open FIFOs and start writing into it
	 */
	conn->curl_handle = curl_easy_init();
	if (!conn->curl_handle) {
		/* something very bad, it should never happen */
		ERROR("FAULT: no handle from libcurl");
		conn->exitval = FAILURE;
		goto curl_thread_exit;
	}

	/*
	 * The 100-expect header is unwanted
	 * drop it
	 */
	headerlist = curl_slist_append(headerlist, no_100_header);

	/*
	 * Setting multipart format
	 */
	conn->form = curl_mime_init(conn->curl_handle);

	 /* Fill in the filename field */
	field = curl_mime_addpart(conn->form);
	if ((curl_mime_name(field, "swupdate-package") != CURLE_OK) ||
	    (curl_mime_type(field, "application/octet-stream") != CURLE_OK) ||
	    (curl_mime_filename(field, "swupdate.swu") != CURLE_OK)) {
		ERROR("curl set MIME was not successful");
		conn->exitval = FAILURE;
		goto curl_thread_exit;
	}

	if ((curl_easy_setopt(conn->curl_handle, CURLOPT_POST, 1L) != CURLE_OK) ||
	   (curl_mime_data_cb(field, conn->total_bytes, curl_read_data,
				NULL, NULL, conn) != CURLE_OK) ||
	    (curl_easy_setopt(conn->curl_handle, CURLOPT_USERAGENT,
		      "libcurl-agent/1.0") != CURLE_OK) ||
	    (curl_easy_setopt(conn->curl_handle, CURLOPT_MIMEPOST,
				      conn->form) != CURLE_OK) ||
	    (curl_easy_setopt(conn->curl_handle, CURLOPT_HTTPHEADER,
			      headerlist) != CURLE_OK)) {
		ERROR("curl set_option was not successful");
		conn->exitval = FAILURE;
		goto curl_thread_exit;
	}

	/* get verbose debug output please */
	curl_easy_setopt(conn->curl_handle, CURLOPT_VERBOSE, 1L);

	/*
	 * Set the URL to post the SWU
	 * This corresponds to the URL set in mongoose interface
	 */
	char *posturl = NULL;
	posturl = (char *)alloca(strlen(conn->url) + strlen(POST_URL_V2) + 1);
	sprintf(posturl, "%s%s", conn->url, POST_URL_V2);

	/* Set URL */
	if (curl_easy_setopt(conn->curl_handle, CURLOPT_URL, posturl) != CURLE_OK) {
		ERROR("Cannot set URL in libcurl");
		conn->exitval = FAILURE;
		goto curl_thread_exit;
	}

	/*
	 * Now perform the transfer
	 */
	CURLcode curlrc = curl_easy_perform(conn->curl_handle);
	if (curlrc != CURLE_OK) {
		ERROR("SWU transfer to %s failed (%d) : '%s'", conn->url, curlrc,
		      curl_easy_strerror(curlrc));
		conn->exitval = FAILURE;
	}

	conn->exitval = SUCCESS;

curl_thread_exit:
	close(conn->fifo[0]);
	curl_easy_cleanup(conn->curl_handle);
	pthread_exit(NULL);
}

static int retrieve_msgs(struct hnd_priv *priv) {
	struct curlconn *conn;
	int ret;
	int result = 0;
	bool finished = false;

	while (!finished) {
		finished = true;
		LIST_FOREACH(conn, &priv->conns, next) {
			if ((conn->connstatus == WS_ESTABLISHED) &&
				(conn->SWUpdateStatus != SUCCESS) &&
				(conn->SWUpdateStatus != FAILURE)) {
				ret = swuforward_ws_getanswer(conn, POLLING_TIME_REQ_STATUS);
				if (ret < 0) {
					conn->SWUpdateStatus = FAILURE;
					break;
				}
				finished = false;
			}
		}
	}

	/*
	 * Now we get results from all connection,
	 * check if all of them were successful
	 */
	result = 0;
	LIST_FOREACH(conn, &priv->conns, next) {
		if (conn->SWUpdateStatus != SUCCESS) {
			ERROR("Update to %s failed !!", conn->url);
			return -EFAULT;
		}
	}

	return result;
}

static int initialize_backchannel(struct hnd_priv *priv)
{
	struct curlconn *conn;
	int ret;

	LIST_FOREACH(conn, &priv->conns, next) {

		ret = swuforward_ws_connect(conn);
		if (!ret) {
			do {
				ret = swuforward_ws_getanswer(conn, POLLING_TIME_REQ_STATUS);
			} while (ret >= 0 && (conn->connstatus == WS_UNKNOWN));
			if (conn->connstatus != WS_ESTABLISHED) {
				ERROR("No connection to %s", conn->url);
				ret = FAILURE;
			}
		}
		if (ret)
			break;
	}

	return ret;
}

static int install_remote_swu(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	struct hnd_priv priv;
	struct curlconn *conn, *tmp;
	int ret;
	struct dict_list_elem *url;
	struct dict_list *urls;
	int index = 0;
	pthread_attr_t attr;
	int thread_ret = -1;

	/*
	 * A single SWU can contains encrypted artifacts,
	 * but the SWU itself cannot be encrypted.
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

	/*
	 * Scan all devices in the list and set up
	 * data structure
	 */
	LIST_FOREACH(url, urls, next) {

		/*
		 * Allocates one structure for connection to download
		 * the SWU to all slaves in parallel
		 */
		conn = (struct curlconn *)calloc(1, sizeof(struct curlconn));
		if (!conn) {
			ERROR("FAULT: no memory");
			ret = -ENOMEM;
			goto handler_exit;
		}

		conn->url = url->value;
		conn->total_bytes = img->size;
		conn->SWUpdateStatus = IDLE;

		LIST_INSERT_HEAD(&priv.conns, conn, next);

		/*
		 * Create one FIFO for each connection to be thread safe
		 */
		if (pipe(conn->fifo) < 0) {
			ERROR("Cannot create internal pipes, exit..");
			ret = FAILURE;
			goto handler_exit;
		}

		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

		thread_ret = pthread_create(&conn->transfer_thread, &attr, curl_transfer_thread, conn);
		if (thread_ret) {
			ERROR("Code from pthread_create() is %d",
				thread_ret);
			conn->transfer_thread = 0;
			ret = FAILURE;
			goto handler_exit;
		}

		index++;
	}

	if (initialize_backchannel(&priv)) {
		ERROR("Cannot initialize back connection");
		ret = FAILURE;
		goto handler_exit;
	}

	ret = copyimage(&priv, img, swu_forward_data);
	if (ret) {
		ERROR("Transferring SWU image was not successful");
		ret = FAILURE;
		goto handler_exit;
	}

	ret = 0;
	LIST_FOREACH(conn, &priv.conns, next) {
		void *status;
		ret = pthread_join(conn->transfer_thread, &status);
		close(conn->fifo[1]);
		if (ret) {
			ERROR("return code from pthread_join() is %d", ret);
			ret = FAILURE;
			goto handler_exit;
		}
		if (conn->exitval != SUCCESS)
			ret = FAILURE;
	}

	/*
	 * Now check if remote updates were successful
	 */
	if (!ret) {
		ret = retrieve_msgs(&priv);
	}

handler_exit:
	LIST_FOREACH_SAFE(conn, &priv.conns, next, tmp) {
		index = 0;
		LIST_REMOVE(conn, next);
		swuforward_ws_free(conn);
		free(conn);
		index++;
	}

	return ret;
}

__attribute__((constructor))
void swuforward_handler(void)
{
	register_handler("swuforward", install_remote_swu,
				IMAGE_HANDLER, NULL);
}
