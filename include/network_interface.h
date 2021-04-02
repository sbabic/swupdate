/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de
 * 	on behalf of ifm electronic GmbH
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _NETWORK_INTERFACE_H
#define _NETWORK_INTERFACE_H

void *network_initializer(void *data);
void *network_thread(void *data);
int listener_create(const char *path, int type);

extern pthread_mutex_t stream_mutex;
extern pthread_cond_t stream_wkup;
#endif
