/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <pthread.h>
#include <network_ipc.h>
#include <pctl.h>
#include <util.h>

/*
 * The global pid is used to identify if context is
 * the main process (SWUpdate, pid=0) or it is 
 * a child process.
 * This is required when using internal libraries and can be
 * decided between a direct call or (in case of child process)
 * a IPC is required.
 */
int pid = 0;

/*
 * This is used to spawn internal threads
 */
pthread_t start_thread(void *(* start_routine) (void *), void *arg)
{
	int ret;
	pthread_t id;
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	ret = pthread_create(&id, &attr, start_routine, arg);
	if (ret) {
		exit(1);
	}
	return id;
}

/*
 * spawn_process forks and start a new process
 * under a new user
 */
int spawn_process(uid_t run_as_userid, gid_t run_as_groupid, const char *cfgname,
	        	int ac, char **av, swupdate_process start)
{
	int process_id;

	process_id = fork();

	/*
	 * if father, it is finished
	 */
	if (process_id)
		return process_id;
	
	/* process is running as root, drop privileges */
	if (getuid() == 0) {
		if (setgid(run_as_groupid) != 0) {
			ERROR("setgid: Unable to drop group privileges: %s", strerror(errno));
			return errno;
		}

		if (setuid(run_as_userid) != 0) {
			ERROR("setuid: Unable to drop user privileges: %s", strerror(errno));
			return errno;
		}
	}

	/* Save new pid */
	pid = getpid();

	notify_init();

	return (*start)(cfgname, ac, av);
}
