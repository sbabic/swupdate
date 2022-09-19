/*
 * Copyright (c) 2004-2013 Sergey Lyubka
 * Copyright (c) 2013-2020 Cesanta Software Limited
 * All rights reserved
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * This software is dual-licensed: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation. For the terms of this
 * license, see <http://www.gnu.org/licenses/>.
 *
 * You are free to use this software under the terms of the GNU General
 * Public License, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * Alternatively, you can license this software under a commercial
 * license, as set out in <https://www.cesanta.com/license>.
 */

#include "mongoose.h"

enum {
	MG_EV_HTTP_MULTIPART_REQUEST=MG_EV_USER + 1,  // struct mg_http_message *
	MG_EV_HTTP_PART_BEGIN,                        // struct mg_http_multipart_part *
	MG_EV_HTTP_PART_DATA,                         // struct mg_http_multipart_part *
	MG_EV_HTTP_PART_END,                          // struct mg_http_multipart_part *
	MG_EV_HTTP_MULTIPART_REQUEST_END              // struct mg_http_multipart_part *
};

/* HTTP multipart part */
struct mg_http_multipart {
	struct mg_http_part part;
	int status; /* <0 on error */
	void *user_data;
	/*
	 * User handler can indicate how much of the data was consumed
	 * by setting this variable. By default, it is assumed that all
	 * data has been consumed by the handler.
	 * If not all data was consumed, user's handler will be invoked again later
	 * with the remainder.
	 */
	size_t num_data_consumed;
	size_t len;
};

void multipart_upload_handler(struct mg_connection *nc, int ev, void *ev_data, void *fn_data);
