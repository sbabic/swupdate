// SPDX-FileCopyrightText: 2026 Fabian Ritter <fabian.ritter@noxsystems.com>
//
// SPDX-License-Identifier: GPL-2.0-only

/*
 * Tests for core/progress_thread.c, the progress interface of SWUpdate.
 *
 * The real progress thread is started on a socket in a temporary
 * directory. The tests are its clients. The thread runs in the test
 * process, so its sockets count as open file descriptors of the test.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "network_utils.h"
#include "pctl.h"
#include "progress.h"
#include "progress_ipc.h"
#include "swupdate.h"
#include "util.h"

static char tmpdir[] = "/tmp/swupdate-test-progthr-XXXXXX";

static long elapsed_ms(struct timespec *start)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

static int count_open_fds(void)
{
	int fd, count = 0;

	for (fd = 0; fd < (int)sysconf(_SC_OPEN_MAX); fd++)
		if (fcntl(fd, F_GETFD) != -1)
			count++;
	return count;
}

/* the thread closes its socket for a client as soon as the client closes its end */
static void test_progthr_closed_client_dropped(void **state)
{
	struct timespec start;
	int clients[5];
	int before;
	unsigned int i;

	(void)state;
	before = count_open_fds();
	for (i = 0; i < ARRAY_SIZE(clients); i++) {
		clients[i] = progress_ipc_connect(false);
		assert_true(clients[i] >= 0);
	}
	/* one socket here and one in the thread per client */
	assert_int_equal(count_open_fds(), before + 2 * ARRAY_SIZE(clients));

	for (i = 0; i < ARRAY_SIZE(clients); i++)
		close(clients[i]);

	clock_gettime(CLOCK_MONOTONIC, &start);
	while (count_open_fds() != before && elapsed_ms(&start) < 5000)
		usleep(10000);
	assert_int_equal(count_open_fds(), before);
}

static volatile bool fail_next_send;

/* send() is wrapped for the whole test, it fails once on request */
extern ssize_t __real_send(int sockfd, const void *buf, size_t len, int flags);
ssize_t __wrap_send(int sockfd, const void *buf, size_t len, int flags);
ssize_t __wrap_send(int sockfd, const void *buf, size_t len, int flags)
{
	if (fail_next_send) {
		fail_next_send = false;
		errno = EIO;
		return -1;
	}
	return __real_send(sockfd, buf, len, flags);
}

/* a failed ACK is logged as an error without blocking on the progress lock */
static void test_progthr_ack_failure(void **state)
{
	int fd;

	(void)state;
	fail_next_send = true;
	alarm(20);
	assert_int_equal(progress_ipc_connect(false), -1);
	/* the thread still serves the next client */
	fd = progress_ipc_connect(false);
	assert_true(fd >= 0);
	alarm(0);
	close(fd);
}

/* a message reaches every client, also one that only shut down its write half */
static void test_progthr_message(void **state)
{
	struct progress_msg msg;
	int client, reader;

	(void)state;
	client = progress_ipc_connect(false);
	assert_true(client >= 0);
	reader = progress_ipc_connect(false);
	assert_true(reader >= 0);
	assert_int_equal(shutdown(reader, SHUT_WR), 0);
	/* time for a wrong server to drop the reader */
	usleep(100000);

	swupdate_progress_init(1);
	alarm(10);
	assert_int_equal(progress_ipc_receive(&client, &msg), sizeof(msg));
	assert_int_equal(msg.status, START);
	assert_int_equal(progress_ipc_receive(&reader, &msg), sizeof(msg));
	assert_int_equal(msg.status, START);
	alarm(0);
	close(client);
	close(reader);
}

static volatile bool stop_logging;

/* the notifiers take the progress lock for every progress event */
static void *log_progress_events(void __attribute__ ((__unused__)) *data)
{
	while (!stop_logging) {
		notify(PROGRESS, 0, TRACELEVEL, "test");
		/* let the progress thread get the locks too */
		usleep(100);
	}
	return NULL;
}

/* clients are dropped while another thread logs progress events */
static void test_progthr_drop_while_logging(void **state)
{
	pthread_t logger;
	int fd;
	unsigned int i;

	(void)state;
	stop_logging = false;
	assert_int_equal(pthread_create(&logger, NULL, log_progress_events, NULL), 0);
	alarm(60);
	for (i = 0; i < 100; i++) {
		fd = progress_ipc_connect(false);
		assert_true(fd >= 0);
		close(fd);
	}
	alarm(0);
	stop_logging = true;
	pthread_join(logger, NULL);
}

/* the socket gets the configured permissions */
static void test_progthr_socket_mode(void **state)
{
	struct stat st;

	(void)state;
	assert_int_equal(stat(get_prog_socket(), &st), 0);
	assert_int_equal(st.st_mode & 07777, 0600);
}

static int progthr_group_setup(void **state)
{
	struct swupdate_cfg *cfg;

	(void)state;
	if (!mkdtemp(tmpdir))
		return -1;
	/* the progress socket is created in TMPDIR */
	setenv("TMPDIR", tmpdir, 1);
	unsetenv("RUNTIME_DIRECTORY");

	/* configuration as set up by SWUpdate at startup */
	cfg = get_swupdate_cfg();
	cfg->progress_socket_mode = 0600;
	cfg->progress_socket_gid = (gid_t)-1;

	/* as at startup: notifiers and the sockets to remove at exit */
	notify_init();
	if (init_socket_unlink_handler())
		return -1;

	start_thread(progress_bar_thread, NULL);
	wait_threads_ready();
	return 0;
}

static int progthr_group_teardown(void **state)
{
	(void)state;
	/* the server keeps running until the test exits */
	unlink(get_prog_socket());
	return rmdir(tmpdir);
}

int main(void)
{
	static const struct CMUnitTest tests[] = {
		/* first, as the baseline of open file descriptors must be clean */
		cmocka_unit_test(test_progthr_closed_client_dropped),
		cmocka_unit_test(test_progthr_ack_failure),
		cmocka_unit_test(test_progthr_message),
		cmocka_unit_test(test_progthr_drop_while_logging),
		cmocka_unit_test(test_progthr_socket_mode),
	};
	return cmocka_run_group_tests_name("progress_thread", tests, progthr_group_setup,
					   progthr_group_teardown);
}
