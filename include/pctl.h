/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _SWUPDATE_PCTL_H
#define _SWUPDATE_PCTL_H

#include <swupdate_status.h>
#include <sys/types.h>

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

void sigchld_handler (int __attribute__ ((__unused__)) signum);

int pctl_getfd_from_type(sourcetype s);
const char *pctl_getname_from_type(sourcetype s);
int run_system_cmd(const char *cmd);

#endif
