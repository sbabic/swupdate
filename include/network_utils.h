/*
 * (C) Copyright 2023
 * Felix Moessbauer <felix.moessbauer@siemens.com>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */
#pragma once

#include <sys/types.h>

/**
 * \brief create or attach to socket at path
 * 
 * \param path absolute path to socket file
 * \param type socket type of socket()
 * \param mode permissions of the socket file
 * \param gid group owner of the socket file, (gid_t)-1 to keep it
 * \note mode and gid are not applied to sockets passed by systemd
 * \return fd on success, -1 on error
*/
int listener_create(const char *path, int type, mode_t mode, gid_t gid);

/**
 * \brief initialize unlink functionality for sockets
 * 
 * Call this function before \c register_socket_unlink
 * \return 0 on success
 */
int init_socket_unlink_handler(void);

/** 
 * \brief unlink socket path on exit
 * 
 * \note threadsafe
 * \return 0 on success
 */
int register_socket_unlink(const char* path);
