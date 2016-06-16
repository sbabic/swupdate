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
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>

#include <progress.h>

#define PSPLASH_MSG_SIZE	64

static int psplash_init(char *pipe)
{
	int psplash_pipe_fd;
	int pid_psplash;

	if ((psplash_pipe_fd = open(pipe, O_WRONLY | O_NONBLOCK)) == -1) {
		/* Try to run psplash in background */
		pid_psplash = fork();
		if (pid_psplash < 0)
			return 0;
		else if (pid_psplash == 0) {
			execl("/usr/bin/psplash", "psplash", (char *)0);
			exit(1);
		} else {
			sleep(1);
			if ((psplash_pipe_fd = open(pipe, O_WRONLY | O_NONBLOCK)) == -1) {
				return 0;
			}
		}
	}

	return 1;
}

static void psplash_write_fifo(char *pipe, char *buf)
{
	int   psplash_pipe_fd, ret;

	if ((psplash_pipe_fd = open(pipe, O_WRONLY | O_NONBLOCK)) == -1) {
		fprintf(stderr, "Error unable to open psplash pipe, closing...\n");
		return;
	}

	buf[strlen(buf)] = '\0';
	ret = write(psplash_pipe_fd, buf, strlen(buf) + 1);
	if (ret < 0) {
		fprintf(stderr, "PSPLASH not available anymore");
	}

	close(psplash_pipe_fd);
}

static void psplash_progress(char *pipe, struct progress_msg *pmsg)
{
	char *buf;

	buf = malloc(PSPLASH_MSG_SIZE);

	if (!buf)
		return;

	if (pmsg->status != START) {
		snprintf(buf, PSPLASH_MSG_SIZE - 1, "MSG %s",
			pmsg->status == SUCCESS ? "SUCCESS" : "FAILURE");
		psplash_write_fifo(pipe, buf);

		sleep(5);

		snprintf(buf, PSPLASH_MSG_SIZE - 1, "QUIT");
		psplash_write_fifo(pipe, buf);
		free(buf);
		return;
	}

	snprintf(buf, PSPLASH_MSG_SIZE - 1, "MSG step %d of %d",
		       	pmsg->cur_step, pmsg->nsteps);
	psplash_write_fifo(pipe, buf);

	usleep(100);

	snprintf(buf, PSPLASH_MSG_SIZE - 1, "PROGRESS %d", pmsg->cur_percent);
	psplash_write_fifo(pipe, buf);

	free(buf);
}

int main(void) {
	int connfd;
	struct sockaddr_un servaddr;
	struct progress_msg msg;
	int ret;
	const char *tmpdir;
	char psplash_pipe_path[256];
	int psplash_ok = 0;
	int curstep = 0;
	int percent = 0;
	char bar[60];
	int filled_len;

	tmpdir = getenv("TMPDIR");
	if (!tmpdir)
		tmpdir = "/tmp";
	snprintf(psplash_pipe_path, sizeof(psplash_pipe_path), "%s/psplash_fifo", tmpdir);

	/*
	 * The thread read from swupdate progress thread
	 * and forward messages to psplash
	 */
	connfd = socket(AF_LOCAL, SOCK_STREAM, 0);
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sun_family = AF_LOCAL;
	strcpy(servaddr.sun_path, SOCKET_PROGRESS_PATH);

	ret = connect(connfd, (struct sockaddr *) &servaddr, sizeof(servaddr));
	if (ret < 0) {
		fprintf(stderr, "no communication with swupdate\n");
	}

	while (1) {
		ret = read(connfd, &msg, sizeof(msg));
		if (ret != sizeof(msg)) {
			close(connfd);
			exit(1);
		}
		if (!psplash_ok) {
			psplash_ok = psplash_init(psplash_pipe_path);
		}

		if ((msg.cur_step != curstep) && (curstep != 0))
			fprintf(stdout, "\n");

		filled_len = sizeof(bar) * msg.cur_percent / 100;
		if (filled_len > sizeof(bar))
			filled_len = sizeof(bar);

		memset(bar,'=', filled_len);
		memset(&bar[filled_len], '-', sizeof(bar) - filled_len);

		fprintf(stdout, "[ %.60s ] %d of %d %d%% (%s)\r",
			bar,
			msg.cur_step, msg.nsteps, msg.cur_percent,
		       	msg.cur_image);
		fflush(stdout);

		if (psplash_ok && ((msg.cur_step != curstep) || (msg.cur_percent != percent))) {
			psplash_progress(psplash_pipe_path, &msg);
			curstep = msg.cur_step;
			percent = msg.cur_percent;
		}

		if (msg.status != START) {
			fprintf(stdout, "\n\n%s !\n", msg.status == SUCCESS ? "SUCCESS" : "FAILURE");
			psplash_progress(psplash_pipe_path, &msg);
			psplash_ok = 0;
		}

	}
}
