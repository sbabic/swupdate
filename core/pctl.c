/*
 * (C) Copyright 2016
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <stdlib.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <errno.h>
#include <string.h>
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

typedef struct {
	int (*exec)(int argc, char **argv);
	int argc;
	char **argv;
} bgtask;

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

/*
 * This allows waiting for initial threads to be ready before spawning subprocesses
 */
static int threads_towait = 0;
static pthread_mutex_t threads_towait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t threads_towait_cond = PTHREAD_COND_INITIALIZER;

#if defined(__linux__)
static void parent_dead_handler(int __attribute__ ((__unused__)) dummy)
{
	_exit(1);
}
#endif

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

	pthread_mutex_lock(&threads_towait_lock);
	threads_towait++;
	pthread_mutex_unlock(&threads_towait_lock);

	ret = pthread_create(&id, &attr, start_routine, arg);
	if (ret) {
		exit(1);
	}
	return id;
}

/*
 * Internal threads should signal they are ready if internal subprocesses
 * can be spawned after them
 */
void thread_ready(void)
{
	pthread_mutex_lock(&threads_towait_lock);
	threads_towait--;
	if (threads_towait == 0)
		pthread_cond_broadcast(&threads_towait_cond);
	pthread_mutex_unlock(&threads_towait_lock);
}

/*
 * Wait for all threads to have signaled they're ready
 */
void wait_threads_ready(void)
{
	pthread_mutex_lock(&threads_towait_lock);
	while (threads_towait != 0)
		pthread_cond_wait(&threads_towait_cond, &threads_towait_lock);
	pthread_mutex_unlock(&threads_towait_lock);
}

/*
 * spawn_process forks and start a new process
 * under a new user
 */
static int spawn_process(struct swupdate_task *task,
			uid_t run_as_userid, gid_t run_as_groupid,
			const char *cfgname,
			int ac, char **av,
			swupdate_process start,
			const char *cmdline)
{
	int process_id;
	int sockfd[2];

	/*
	 * Create the pipe to exchange data with the child
	 */
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockfd) < 0) {
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

#if defined(__linux__)
	if (signal(SIGUSR1, parent_dead_handler) == SIG_ERR) {
		/*
		 * this is not a reason to break, just a warning
		 */
		WARN("Cannot track if parent dies, sorry...");
	}

	if (prctl(PR_SET_PDEATHSIG, SIGUSR1) < 0)
		ERROR("Error calling prctl");
#else
	WARN("Cannot track if parent dies on non-Linux OSes, sorry...");
#endif

	if (start)
		return (*start)(cfgname, ac, av);
	else {
		if (execvp(cmdline, av) == -1) {
			INFO("Spawning process %s failed: %s", av[0], strerror(errno));
			return -1;
		}
		return 0;
	}
}

static void start_swupdate_subprocess(sourcetype type, const char *name,
			uid_t run_as_userid, gid_t run_as_groupid,
			const char* cfgfile,
			int argc, char **argv,
			swupdate_process start,
			const char *cmdline)
{
	procs[nprocs].name = name;
	procs[nprocs].type = type;
	if (spawn_process(&procs[nprocs], run_as_userid, run_as_groupid, cfgfile, argc, argv, start, cmdline) < 0) {
		ERROR("Spawning %s failed, exiting process...", name);
		exit(1);
	}

	TRACE("Started %s with pid %d and fd %d", name, procs[nprocs].pid, procs[nprocs].pipe);
	nprocs++;
}


void start_subprocess_from_file(sourcetype type, const char *name,
			uid_t run_as_userid, gid_t run_as_groupid,
			const char *cfgfile,
			int argc, char **argv,
			const char *cmdline)
{
	start_swupdate_subprocess(type, name, run_as_userid, run_as_groupid, cfgfile, argc, argv, NULL, cmdline);
}

void start_subprocess(sourcetype type, const char *name,
			uid_t run_as_userid, gid_t run_as_groupid,
			const char *cfgfile,
			int argc, char **argv,
			swupdate_process start)
{

	start_swupdate_subprocess(type, name, run_as_userid, run_as_groupid, cfgfile, argc, argv, start, NULL);
}

/*
 * run_cmd executes a shell script or an internal function in background
 * in a separate process and intercepts stdout and stderr, writing then to
 * TRACE and ERROR.
 * This let the output of the scripts / functions to be collected by SWUpdate
 * tracing capabilities.
 */
static int __run_cmd(const char *cmd, bgtask *fn)
{
	int ret = 0;
	int const npipes = 4;
	int pipes[npipes][2];
	LOGLEVEL levels[4] = { TRACELEVEL, ERRORLEVEL, INFOLEVEL, WARNLEVEL };
	pid_t process_id;
	int const PIPE_READ = 0;
	int const PIPE_WRITE = 1;
	int wstatus, i;
	bool execute_function = !(cmd && strnlen(cmd, SWUPDATE_GENERAL_STRING_SIZE)) && fn;

	/*
	 * There are two cases:
	 * - an external command should be executed (cmd is not NULL)
	 * - an internal SWUpdate function must be executed in bg process
	 * Both cannot be executed, the external command has priority
	 */

	/*
	 * Check parameter in case of external command or internal function
	 * that should run in a separate process
	 */
	if (!execute_function) {
		/*
		 * If no cmd and function are passed, just return without error
		 * like "nothing was successfully executed"
		 */
		if (!cmd)
			return 0;
		if (!strnlen(cmd, SWUPDATE_GENERAL_STRING_SIZE))
			return 0;
		if (strnlen(cmd, SWUPDATE_GENERAL_STRING_SIZE) > SWUPDATE_GENERAL_STRING_SIZE) {
			ERROR("Command string too long, skipping..");
			/* do nothing */
			return -EINVAL;
		}
	}

	/*
	 * Creates pipes to intercept stdout and stderr of the
	 * child process
	 */
	for (i = 0; i < npipes; i++) {
		if (pipe(pipes[i]) < 0) {
			ERROR("Could not create pipes for subprocess, existing...");
			break;
		}
	}
	if (i < npipes) {
		while (i >= 0) {
			close(pipes[i][0]);
			close(pipes[i][1]);
			i--;
		}
		return -EFAULT;
	}

	process_id = fork();
	/* Child process, this runs the shell command */
	if (process_id == 0) {
		if (dup2(pipes[0][PIPE_WRITE], STDOUT_FILENO) < 0)
			exit(errno);
		if (dup2(pipes[1][PIPE_WRITE], STDERR_FILENO) < 0)
			exit(errno);
		/* posix sh cannot use fd >= 10, so dup these to lower
		 * numbers for convenience */
		if (dup2(pipes[2][PIPE_WRITE], 3) < 0)
			exit(errno);
		setenv("SWUPDATE_INFO_FD", "3", 1);
		if (dup2(pipes[3][PIPE_WRITE], 4) < 0)
			exit(errno);
		setenv("SWUPDATE_WARN_FD", "4", 1);

		/* close all pipes, not used anymore */
		for (i = 0; i < npipes; i++) {
			close(pipes[i][PIPE_READ]);
			close(pipes[i][PIPE_WRITE]);
		}

		if (!execute_function) {
			ret = execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
			if (ret) {
				ERROR("Process %s cannot be started: %s", cmd, strerror(errno));
				exit(1);
			}
		} else {
			ret = fn->exec(fn->argc, fn->argv);
			exit(ret);
		}
	} else {
		int fds[npipes];
		int fdmax = 0;
		pid_t w;
		/*
		 * Use buffers (for stdout and stdin) to collect data from
		 * the cmd. Data can contain multiple lines or just a part
		 * of a line and must be parsed
		 */
		char buf[npipes][SWUPDATE_GENERAL_STRING_SIZE];
		int cindex[npipes];

		for (i = 0; i < npipes; i++) {
			close(pipes[i][PIPE_WRITE]);
			fds[i] = pipes[i][PIPE_READ];
			memset(buf[i], 0, sizeof(buf[i]));
			cindex[i] = 0;
			if (fds[i] > fdmax) fdmax = fds[i];
		}

		/*
		 * Now waits until the child process exits and checks
		 * for the output. Forward data from stdout as TRACE
		 * and from stderr (of the child process) as ERROR
		 */
		do {
			int n1 = 0;
			struct timeval tv;
			fd_set readfds;
			int n;

			w = waitpid(process_id, &wstatus, WNOHANG);
			if (w == -1) {
				ERROR("Error from waitpid() !!");
				close(pipes[0][PIPE_READ]);
				close(pipes[1][PIPE_READ]);
				return -EFAULT;
			}

			tv.tv_sec = 1;
			tv.tv_usec = 0;

			/* Check if the child has sent something */
			do {
				FD_ZERO(&readfds);
				for (i = 0; i < npipes; i++)
					FD_SET(fds[i], &readfds);

				n1 = 0;
				ret = select(fdmax + 1, &readfds, NULL, NULL, &tv);
				if (ret <= 0)
					break;

				for (i = 0; i < npipes ; i++) {
					char *pbuf = buf[i];
					int *c = &cindex[i];
					LOGLEVEL level = levels[i];

					if (FD_ISSET(fds[i], &readfds)) {
						n = read_lines_notify(fds[i], pbuf, SWUPDATE_GENERAL_STRING_SIZE,
								      c, level);
						if (n < 0)
							continue;
						n1 += n;
					}
				}
			} while (ret > 0 && n1 > 0);
		} while (w != process_id);

		/* print any unfinished line */
		for (i = 0; i < npipes; i++) {
			if (cindex[i]) {
				switch(i) {
				case 0:
					TRACE("%s", buf[i]);
					break;
				case 1:
					ERROR("%s", buf[i]);
					break;
				}
			}
			close(pipes[i][PIPE_READ]);
		}

		if (WIFEXITED(wstatus)) {
			ret = WEXITSTATUS(wstatus);
			TRACE("%s command returned %d", cmd ? cmd : "", ret);
		} else if (WIFSIGNALED(wstatus)) {
			TRACE("(%s) killed by signal %d\n", cmd ? : "", WTERMSIG(wstatus));
			ret = -1;
		} else {
			TRACE("(%s) not exited nor killed!\n", cmd ? cmd : "");
			ret = -1;
		}
	}

	return ret;
}

int run_system_cmd(const char *cmd) {
	return __run_cmd(cmd, NULL);
}

int run_function_background(void *fn, int argc, char **argv) {
	bgtask bg;

	bg.exec = fn;
	bg.argc = argc;
	bg.argv = argv;

	return __run_cmd(NULL, &bg);
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
	int hasdied = 0;
	int i;

	serrno = errno;

	/*
	 * One process stops, find who is
	 */
	for (i = 0; i < nprocs; i++) {
		childpid = waitpid (procs[i].pid, &status, WNOHANG);
		if (childpid < 0) {
			perror ("waitpid, no child");
			continue;
		}
		if (childpid == 0)
			continue;

		if (procs[i].pid == childpid) {
			printf("Child %d(%s) ", childpid, procs[i].name);
			hasdied = 0;
			if (WIFEXITED(status)) {
				hasdied = 1;
				exit_code = WEXITSTATUS(status);
				printf("exited, status=%d\n", exit_code);
			} else if (WIFSIGNALED(status)) {
				hasdied = 1;
				exit_code = EXIT_FAILURE;
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

		/*
		 * exit() it not safe to call from a signal handler because of atexit()
		 * handlers, so send SIGTERM to ourself instead
		 */
		kill(getpid(), SIGTERM);
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

void *ipc_thread_fn(void *data)
{
	fd_set readfds;
	int retval;
	server_ipc_fn fn = (server_ipc_fn)data;

	while (1) {
		FD_ZERO(&readfds);
		FD_SET(sw_sockfd, &readfds);
		retval = select(sw_sockfd + 1, &readfds, NULL, NULL, NULL);

		if (retval < 0) {
			TRACE("IPC awakened because of: %s", strerror(errno));
			return 0;
		}

		if (retval && FD_ISSET(sw_sockfd, &readfds)) {
			if (fn(sw_sockfd) != SERVER_OK) {
				DEBUG("Handling IPC failed!");
			}
		}
	}
}
