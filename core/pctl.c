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
#include <sys/socket.h>
#include <sys/prctl.h>
#include <errno.h>
#include <pthread.h>
#include <network_ipc.h>
#include <pctl.h>
#include <util.h>
#include <signal.h>
#include <sys/wait.h>
#include <parselib.h>
#include <swupdate_settings.h>

#ifndef WAIT_ANY
#define WAIT_ANY (-1)
#endif

/* the array contains the pid of the subprocesses */
#define MAX_PROCESSES	10
static struct swupdate_task procs[MAX_PROCESSES];
static int    nprocs = 0;

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
 * The sw_sockfd is used for internal ipc
 * with SWUpdate processes
 */

int sw_sockfd = -1;

static void parent_dead_handler(int dummy)
{
	dummy = 0;
	exit(1);
}

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
int spawn_process(struct swupdate_task *task,
			uid_t run_as_userid, gid_t run_as_groupid,
			const char *cfgname,
	        	int ac, char **av, swupdate_process start)
{
	int process_id;
	int sockfd[2];


	/*
	 * Create the pipe to exchange data with the child
	 */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockfd) < 0) {
		ERROR("socketpair fails : %s", strerror(errno));
		return -1;
	}

	process_id = fork();


	/*
	 * We need just one bidirectional pipe
	 */

	/*
	 * if father, it is finished
	 */
	if (process_id) {
		/* Parent close the [1] */
		close(sockfd[1]);
		task->pid = process_id;
		task->pipe = sockfd[0];
		return 0;
	}
	
	/* Child closes [0] */
	close(sockfd[0]);

	sw_sockfd = sockfd[1];

	/* process is running as root, drop privileges */
	if (getuid() == 0) {
		if (setgid(run_as_groupid) != 0) {
			ERROR("setgid: Unable to drop group privileges: %s", strerror(errno));
			return -1;
		}

		if (setuid(run_as_userid) != 0) {
			ERROR("setuid: Unable to drop user privileges: %s", strerror(errno));
			return -1;
		}
	}

	/* Save new pid */
	pid = getpid();

	notify_init();

	if (signal(SIGUSR1, parent_dead_handler) == SIG_ERR) {
		/*
		 * this is not a reason to break, just a warning
		 */
		WARN("Cannot track if parent dies, sorry...");
	}

	if (prctl(PR_SET_PDEATHSIG, SIGUSR1) < 0)
		ERROR("Fail to call prctl, maybe not Linux ?");


	return (*start)(cfgname, ac, av);
}

void start_subprocess(sourcetype type, const char *name, const char *cfgfile,
			int argc, char **argv,
			swupdate_process start)
{
	uid_t uid;
	gid_t gid;

	read_settings_user_id(cfgfile, name, &uid, &gid);
	procs[nprocs].name = name;
	procs[nprocs].type = type;
	if (spawn_process(&procs[nprocs], uid, gid, cfgfile, argc, argv, start) < 0) {
		ERROR("SPAWN %s failed, exiting...\n", name);
		return;
	}

	TRACE("Started %s with pid %d and fd %d", name, procs[nprocs].pid, procs[nprocs].pipe);
	nprocs++;
}

/*
 * The handler supervises the subprocesses
 * (Downloader, Webserver, Suricatta)
 * if one of them dies, SWUpdate exits
 * and sends a SIGTERM signal to all other subprocesses
 */
void sigchld_handler (int __attribute__ ((__unused__)) signum)
{
	int childpid, status, serrno;
	int exitstatus;
	int hasdied = 0;
	int i;

	serrno = errno;

	/*
	 * One process stops, find who is
	 */
	for (i = 0; i < nprocs; i++) {
		childpid = waitpid (procs[i].pid, &status, WNOHANG);
		if (childpid < 0) {
			perror ("waitpid, no childs");
			continue;
		}
		if (childpid == 0)
			continue;

		if (procs[i].pid == childpid) {
			printf("Child %d(%s) ", childpid, procs[i].name);
			hasdied = 0;
			if (WIFEXITED(status)) {
				hasdied = 1;
				exitstatus = WEXITSTATUS(status);
				printf("exited, status=%d\n", exitstatus);
			} else if (WIFSIGNALED(status)) {
				hasdied = 1;
				exitstatus = WTERMSIG(status);
				printf("killed by signal %d\n", WTERMSIG(status));
			} else if (WIFSTOPPED(status)) {
				printf("stopped by signal %d\n", WSTOPSIG(status));
			} else if (WIFCONTINUED(status)) {
				printf("continued\n");
			}
			break;
		}
	}

	/*
	 * Communicate to all other processes that something happened
	 * and exit
	 */
	if (hasdied) {
		signal(SIGCHLD, SIG_IGN);
		for (i = 0; i < nprocs; i++) {
			if (procs[i].pid != childpid) {
				kill(procs[i].pid, SIGTERM);
			}
		}

		exit(exitstatus);
	}

	errno = serrno;
}

int pctl_getfd_from_type(sourcetype s)
{
	int i;

	for (i = 0; i < nprocs; i++) {
		if (s == procs[i].type)
			return procs[i].pipe;
	}

	return -ENODEV;
}

const char *pctl_getname_from_type(sourcetype s)
{
	int i;

	for (i = 0; i < nprocs; i++) {
		if (s == procs[i].type)
			return procs[i].name;
	}

	return "";
}
