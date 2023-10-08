/*
 * (C) Copyright 2016-2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <swupdate_status.h>
#include <sys/types.h>
#include "util.h"

extern int pid;
extern int sw_sockfd;

/*
 * This is used by the core process
 * to monitor all derived processes
 */
struct swupdate_task {
	pid_t	pid;
	int	pipe;
	sourcetype	type;
	const char	*name;
};

pthread_t start_thread(void *(* start_routine) (void *), void *arg);

void thread_ready(void);
void wait_threads_ready(void);

typedef int (*swupdate_process)(const char *cfgname, int argc, char **argv);
typedef server_op_res_t(*server_ipc_fn)(int fd);

void start_subprocess(sourcetype type, const char *name,
			uid_t run_as_userid, gid_t run_as_groupid,
			const char *cfgfile,
			int argc, char **argv,
			swupdate_process start);

void start_subprocess_from_file(sourcetype type, const char *name,
			uid_t run_as_userid, gid_t run_as_groupid,
			const char *cfgfile,
			int argc, char **argv,
			const char *cmd);
/* ipc_thread listens to pctl IPC socket and call server specific function */
void *ipc_thread_fn(void *data);

void sigchld_handler (int __attribute__ ((__unused__)) signum);

int pctl_getfd_from_type(sourcetype s);
const char *pctl_getname_from_type(sourcetype s);
int run_system_cmd(const char *cmd);
int run_function_background(void *fn, int argc, char **argv);
