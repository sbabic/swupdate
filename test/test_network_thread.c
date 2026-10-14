// SPDX-FileCopyrightText: 2026 Stefano Babic <stefano.babic@swupdate.org>
//
// SPDX-License-Identifier: GPL-2.0-only

/*
 * Tests for core/network_thread.c, the IPC server of SWUpdate.
 *
 * The real network thread is started on a socket in a temporary
 * directory. The tests are its clients, with the IPC library or with
 * raw sockets for malformed or misbehaving clients.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "generated/autoconf.h"
#include "bootloader.h"
#include "installer_priv.h"
#include "network_interface.h"
#include "network_utils.h"
#include "network_ipc.h"
#include "pctl.h"
#include "state.h"
#include "swupdate.h"
#include "swupdate_dict.h"
#include "util.h"

extern bool stream_wkup;
extern pthread_mutex_t stream_mutex;
char *swupdate_get_decrypt_key(void);
char swupdate_get_decrypt_keylen(void);

static char tmpdir[] = "/tmp/swupdate-test-netthr-XXXXXX";
static struct installer inst;
static struct swupdate_cfg *cfg;
static struct swupdate_type_cfg update_type;

/* bootloader environment for the update state */
static struct dict bootenv;

static char *test_env_get(const char *name)
{
	char *v = dict_get_value(&bootenv, name);

	return v ? strdup(v) : NULL;
}

static int test_env_set(const char *name, const char *value)
{
	return dict_set_value(&bootenv, name, value);
}

static int test_env_unset(const char *name)
{
	dict_remove(&bootenv, name);
	return 0;
}

static int test_apply_list(const char __attribute__ ((__unused__)) *script)
{
	return 0;
}

static bootloader test_bootloader = {
	.env_get = test_env_get,
	.env_set = test_env_set,
	.env_unset = test_env_unset,
	.apply_list = test_apply_list,
};

/* elapsed time in ms */
static long elapsed_ms(struct timespec *start)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

static int raw_connect(void)
{
	struct sockaddr_un addr = { .sun_family = AF_LOCAL };
	int fd = socket(AF_LOCAL, SOCK_STREAM, 0);

	assert_true(fd >= 0);
	strlcpy(addr.sun_path, get_ctrl_socket(), sizeof(addr.sun_path));
	assert_int_equal(connect(fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
	return fd;
}

/* send a message and read the answer, with a raw socket */
static int raw_request(ipc_message *msg)
{
	int fd = raw_connect();
	int ret;

	assert_int_equal(write(fd, msg, sizeof(*msg)), sizeof(*msg));
	ret = read(fd, msg, sizeof(*msg));
	close(fd);
	return ret;
}

static void new_msg(ipc_message *msg, msgtype type)
{
	memset(msg, 0, sizeof(*msg));
	msg->magic = IPC_MAGIC;
	msg->type = type;
}

static void test_netthr_status(void **state)
{
	(void)state;
	ipc_message msg;

	inst.status = IDLE;
	inst.last_install = SUCCESS;
	assert_int_equal(ipc_get_status(&msg), 0);
	assert_int_equal(msg.type, ACK);
	assert_int_equal(msg.data.status.last_result, SUCCESS);

	/* notifications are queued and returned one after the other */
	ERROR("first queued message");
	ERROR("second queued message");
	do {
		assert_int_equal(ipc_get_status(&msg), 0);
	} while (strlen(msg.data.status.desc) && !strstr(msg.data.status.desc, "first queued"));
	assert_non_null(strstr(msg.data.status.desc, "first queued"));
	assert_int_equal(ipc_get_status(&msg), 0);
	assert_non_null(strstr(msg.data.status.desc, "second queued"));

	/* with timeout, also longer than one second */
	assert_int_equal(ipc_get_status_timeout(&msg, 200), sizeof(msg));
	assert_int_equal(msg.type, ACK);
	assert_int_equal(ipc_get_status_timeout(&msg, 1500), sizeof(msg));
	assert_int_equal(msg.type, ACK);
}

static void test_netthr_malformed(void **state)
{
	(void)state;
	ipc_message msg;
	int fd;

	/* wrong magic */
	new_msg(&msg, GET_STATUS);
	msg.magic = 0x12345678;
	assert_int_equal(raw_request(&msg), sizeof(msg));
	assert_int_equal(msg.type, NACK);

	/* unknown request */
	new_msg(&msg, 9999);
	assert_int_equal(raw_request(&msg), sizeof(msg));
	assert_int_equal(msg.type, NACK);

	/* too short: closed without answer */
	fd = raw_connect();
	assert_int_equal(write(fd, &msg, 10), 10);
	shutdown(fd, SHUT_WR);
	assert_int_equal(read(fd, &msg, sizeof(msg)), 0);
	close(fd);

	/* the server still works */
	assert_int_equal(ipc_get_status(&msg), 0);
	assert_int_equal(msg.type, ACK);
}

static void test_netthr_silent_client(void **state)
{
	(void)state;
	struct timespec start;
	ipc_message msg;
	int silent;

	/* a client that connects and sends nothing must not block the server */
	silent = raw_connect();
	clock_gettime(CLOCK_MONOTONIC, &start);
	alarm(60);
	assert_int_equal(ipc_get_status(&msg), 0);
	alarm(0);
	assert_int_equal(msg.type, ACK);
	assert_true(elapsed_ms(&start) < 15000);
	close(silent);
}

static void test_netthr_install_request(void **state)
{
	(void)state;
	struct swupdate_request req;
	ipc_message msg;
	int fd;

	/* accepted: the installer is woken up and gets the connection */
	inst.status = IDLE;
	swupdate_prepare_req(&req);
	strlcpy(req.info, "test info", sizeof(req.info));
	fd = ipc_inst_start_ext(&req, sizeof(req));
	assert_true(fd >= 0);
	pthread_mutex_lock(&stream_mutex);
	assert_true(stream_wkup);
	stream_wkup = false;
	assert_string_equal(inst.req.info, "test info");
	close(inst.fd);
	pthread_mutex_unlock(&stream_mutex);
	close(fd);

	/* an installation is running */
	inst.status = RUN;
	assert_true(ipc_inst_start_ext(&req, sizeof(req)) < 0);
	inst.status = IDLE;

	/* wrong API version */
	new_msg(&msg, REQ_INSTALL);
	swupdate_prepare_req(&msg.data.instmsg.req);
	msg.data.instmsg.req.apiversion = 0x4242;
	assert_int_equal(raw_request(&msg), sizeof(msg));
	assert_int_equal(msg.type, NACK);

	/* selection: only the accepted ones */
	swupdate_prepare_req(&req);
	strlcpy(req.software_set, "stable", sizeof(req.software_set));
	strlcpy(req.running_mode, "alt", sizeof(req.running_mode));
	assert_true(ipc_inst_start_ext(&req, sizeof(req)) < 0);
	dict_insert_value(&cfg->accepted_set, "accepted", "stable,alt");
	fd = ipc_inst_start_ext(&req, sizeof(req));
	assert_true(fd >= 0);
	pthread_mutex_lock(&stream_mutex);
	stream_wkup = false;
	close(inst.fd);
	pthread_mutex_unlock(&stream_mutex);
	close(fd);
	dict_drop_db(&cfg->accepted_set);

	/* strings from the client are terminated */
	new_msg(&msg, REQ_INSTALL);
	swupdate_prepare_req(&msg.data.instmsg.req);
	memset(msg.data.instmsg.req.info, 'i', sizeof(msg.data.instmsg.req.info));
	fd = raw_connect();
	assert_int_equal(write(fd, &msg, sizeof(msg)), sizeof(msg));
	assert_int_equal(read(fd, &msg, sizeof(msg)), sizeof(msg));
	assert_int_equal(msg.type, ACK);
	pthread_mutex_lock(&stream_mutex);
	stream_wkup = false;
	assert_int_equal(strlen(inst.req.info), sizeof(inst.req.info) - 1);
	close(inst.fd);
	pthread_mutex_unlock(&stream_mutex);
	close(fd);
}

static void test_netthr_notify(void **state)
{
	(void)state;
	ipc_message msg;
	int fd;

	/* history and new notifications */
	ERROR("message before connecting");
	fd = ipc_notify_connect();
	assert_true(fd >= 0);
	bool found = false;
	ERROR("message after connecting");
	for (int i = 0; i < 200 && !found; i++) {
		assert_true(ipc_notify_receive(&fd, &msg) > 0);
		found = strstr(msg.data.notify.msg, "message after connecting") != NULL;
	}
	assert_true(found);
	close(fd);

	/* the closed client is removed */
	ERROR("message for nobody");
	assert_int_equal(ipc_get_status(&msg), 0);
}

struct flood {
	int count;
	bool done;
};

static void *flood_thread(void *data)
{
	struct flood *f = data;

	/* enough to fill the socket buffer of the client many times */
	for (f->count = 0; f->count < 1000; f->count++)
		ERROR("flood message %d", f->count);
	f->done = true;
	return NULL;
}

static void test_netthr_notify_stuck_client(void **state)
{
	(void)state;
	struct timespec start;
	struct flood f = { 0 };
	pthread_t t;
	ipc_message msg;
	int fd;

	/* a client that does not read must not block the notifications */
	fd = ipc_notify_connect();
	assert_true(fd >= 0);

	/* the console notifier prints errors: keep the flood out of the output */
	fflush(stderr);
	int saved_stderr = dup(STDERR_FILENO);
	int devnull = open("/dev/null", O_WRONLY);
	assert_true(saved_stderr >= 0 && devnull >= 0);
	dup2(devnull, STDERR_FILENO);
	close(devnull);

	clock_gettime(CLOCK_MONOTONIC, &start);
	assert_int_equal(pthread_create(&t, NULL, flood_thread, &f), 0);
	while (!f.done && elapsed_ms(&start) < 20000)
		usleep(10000);

	fflush(stderr);
	dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);

	if (!f.done) {
		int sent = f.count;

		/* closing the client unblocks the server, the next tests can run */
		close(fd);
		pthread_join(t, NULL);
		fail_msg("logging is blocked by a notify client that does not read (%d messages sent)",
			 sent);
	}
	pthread_join(t, NULL);

	/* the server still works */
	assert_int_equal(ipc_get_status(&msg), 0);
	close(fd);
}

static void test_netthr_aes_key(void **state)
{
	(void)state;
	static char key[] = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
	static char ivt[] = "0102030405060708090a0b0c0d0e0f10";
	static char bad[] = "0011";
	static char pkcs11[] = "pkcs11";

	assert_int_equal(swupdate_set_aes(key, ivt), 0);
	assert_int_equal(swupdate_get_decrypt_keylen(), 32);
	assert_int_not_equal(swupdate_set_aes(bad, ivt), 0);
	assert_int_not_equal(swupdate_set_aes(key, bad), 0);
	/* a pkcs11 URI cannot be set via IPC */
	assert_int_not_equal(swupdate_set_aes(pkcs11, ivt), 0);
}

static void test_netthr_hw_revision(void **state)
{
	(void)state;
	ipc_message msg;

	new_msg(&msg, GET_HW_REVISION);
	assert_int_equal(raw_request(&msg), sizeof(msg));
	assert_int_equal(msg.type, ACK);
	assert_string_equal(msg.data.revisions.boardname, "testboard");
	assert_string_equal(msg.data.revisions.revision, "1.0");
}

static void test_netthr_update_state(void **state)
{
	(void)state;
	ipc_message msg;

	new_msg(&msg, SET_UPDATE_STATE);
	msg.data.msg[0] = STATE_INSTALLED;
	assert_int_equal(raw_request(&msg), sizeof(msg));
	assert_int_equal(msg.type, ACK);
	new_msg(&msg, GET_UPDATE_STATE);
	assert_int_equal(raw_request(&msg), sizeof(msg));
	assert_int_equal(msg.type, ACK);
	assert_int_equal(msg.data.msg[0], STATE_INSTALLED);

	/* invalid state */
	new_msg(&msg, SET_UPDATE_STATE);
	msg.data.msg[0] = 'x';
	assert_int_equal(raw_request(&msg), sizeof(msg));
	assert_int_equal(msg.type, NACK);
}

static void test_netthr_delta_url(void **state)
{
	(void)state;

	assert_int_equal(swupdate_dwl_url("rootfs.zck", "http://server/rootfs.zck"), 0);
	assert_string_equal(dict_get_value(&cfg->external_urls, "rootfs.zck"),
			    "http://server/rootfs.zck");
}

static void test_netthr_subprocess(void **state)
{
	(void)state;
	ipc_message msg;

	/* no such subprocess: NACK, and the server goes on */
	new_msg(&msg, SWUPDATE_SUBPROCESS);
	msg.data.procmsg.source = SOURCE_SURICATTA;
	msg.data.procmsg.cmd = CMD_GET_STATUS;
	msg.data.procmsg.len = 100000;
	memset(msg.data.procmsg.buf, 'b', sizeof(msg.data.procmsg.buf));
	assert_int_equal(raw_request(&msg), sizeof(msg));
	assert_int_equal(msg.type, NACK);
	assert_int_equal(ipc_get_status(&msg), 0);
}

static void test_netthr_postupdate(void **state)
{
	(void)state;
	ipc_message msg;
	char cmd[256];

	snprintf(cmd, sizeof(cmd), "touch %s/postupdate-done", tmpdir);
	strlcpy(update_type.postupdatecmd, cmd, sizeof(update_type.postupdatecmd));
	memset(&msg, 0, sizeof(msg));
	assert_int_equal(ipc_postupdate(&msg), 0);
	assert_int_equal(msg.type, ACK);
	snprintf(cmd, sizeof(cmd), "%s/postupdate-done", tmpdir);
	assert_int_equal(access(cmd, F_OK), 0);
	unlink(cmd);

	strlcpy(update_type.postupdatecmd, "exit 1", sizeof(update_type.postupdatecmd));
	memset(&msg, 0, sizeof(msg));
	assert_int_equal(ipc_postupdate(&msg), 0);
	assert_int_equal(msg.type, NACK);
	update_type.postupdatecmd[0] = '\0';
}

static int netthr_group_setup(void **state)
{
	(void)state;
	if (!mkdtemp(tmpdir))
		return -1;
	/* the control socket is created in TMPDIR */
	setenv("TMPDIR", tmpdir, 1);
	unsetenv("RUNTIME_DIRECTORY");

	/* configuration as set up by SWUpdate at startup */
	cfg = get_swupdate_cfg();
	LIST_INIT(&cfg->images);
	LIST_INIT(&cfg->scripts);
	LIST_INIT(&cfg->bootloader);
	LIST_INIT(&cfg->external_urls);
	LIST_INIT(&cfg->accepted_set);
	LIST_INIT(&cfg->swupdate_types);
	strlcpy(update_type.type_name, "default", sizeof(update_type.type_name));
	LIST_INSERT_HEAD(&cfg->swupdate_types, &update_type, next);
	cfg->update_type = &update_type;
	cfg->ctrl_socket_mode = 0600;
	cfg->ctrl_socket_gid = (gid_t)-1;
	strlcpy(cfg->hw.boardname, "testboard", sizeof(cfg->hw.boardname));
	strlcpy(cfg->hw.revision, "1.0", sizeof(cfg->hw.revision));

	LIST_INIT(&bootenv);
	if (register_bootloader("testbl", &test_bootloader) || set_bootloader("testbl"))
		return -1;

	/* as at startup: notifiers and the sockets to remove at exit */
	notify_init();
	if (init_socket_unlink_handler())
		return -1;

	inst.status = IDLE;
	inst.software = cfg;
	start_thread(network_thread, &inst);
	wait_threads_ready();
	return 0;
}

static int netthr_group_teardown(void **state)
{
	char p[256];

	(void)state;
	/* the server keeps running until the test exits */
	snprintf(p, sizeof(p), "%s/sockinstctrl", tmpdir);
	unlink(p);
	return rmdir(tmpdir);
}

int main(void)
{
	static const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_netthr_status),
		cmocka_unit_test(test_netthr_malformed),
		cmocka_unit_test(test_netthr_silent_client),
		cmocka_unit_test(test_netthr_install_request),
		cmocka_unit_test(test_netthr_notify),
		cmocka_unit_test(test_netthr_notify_stuck_client),
		cmocka_unit_test(test_netthr_aes_key),
		cmocka_unit_test(test_netthr_hw_revision),
		cmocka_unit_test(test_netthr_update_state),
		cmocka_unit_test(test_netthr_delta_url),
		cmocka_unit_test(test_netthr_subprocess),
		cmocka_unit_test(test_netthr_postupdate),
	};
	return cmocka_run_group_tests_name("network_thread", tests, netthr_group_setup,
					   netthr_group_teardown);
}
