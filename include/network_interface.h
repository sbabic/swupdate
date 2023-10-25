/*
 * (C) Copyright 2013-2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once
void *network_initializer(void *data);
void *network_thread(void *data);

extern bool stream_wkup;
extern pthread_mutex_t stream_mutex;
extern pthread_cond_t stream_cond;
