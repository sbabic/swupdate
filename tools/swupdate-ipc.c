/*
 * (C) Copyright 2021
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * This is a simple example how to send a command to
 * a SWUpdate's subprocess. It sends a "feedback"
 * to the suricatta module and waits for the answer.
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
#include <sys/reboot.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <pthread.h>
#include <getopt.h>
#include <json-c/json.h>

#include "network_ipc.h"
#include <progress_ipc.h>

struct cmd_t;
typedef void (*help)(const char *name);
typedef int (*cmdfunc)(struct cmd_t *cmd, int argc, char *argv[]);

typedef struct cmd_t {
	const char *name;
	cmdfunc fn;
	help usage;
} cmd_t;

/*
 * usage functions for each command
 */
static void usage_aes(const char *program) {
	fprintf(stdout, "\t %s <key> <ivt>\n", program);
}

static void usage_gethawkbitstatus(const char *program) {
	fprintf(stdout, "\t %s\n", program);
}

static void usage_setversion(const char *program) {
	fprintf(stdout, "\t %s <minversion> <maxversion> <current>\n", program);
}

static void usage_send_to_hawkbit(const char *program) {
	fprintf(stdout, "\t %s <action id> <status> <finished> "
			"<execution> <detail 1> <detail 2> ..\n", program);
}

static void usage_sysrestart(const char *programname)
{
	fprintf(stdout, "\t %s [OPTION]\n", programname);
	fprintf(stdout,
		"\t\t-w, --wait              : wait for a connection with SWUpdate\n"
		"\t\t-s, --socket <path>     : path to progress IPC socket\n"
		"\t\t-h, --help              : print this help and exit\n"
		);
}

static void usage_hawkbitcfg(const char *program) {
	fprintf(stdout,"\t %s \n", program);
	fprintf(stdout,
		"\t\t-p, --polling-time      : Set polling time (0=from server) to ask the backend server\n"
		"\t\t-e, --enable            : Enable polling of backend server\n"
		"\t\t-d, --disable           : Disable polling of backend server\n"
		"\t\t-t, --trigger           : Enable and check for update\n"
		);
}

static void usage_monitor(const char *program) {
	fprintf(stdout,"\t %s \n", program);
	fprintf(stdout,
		"\t\t-s, --socket <path>     : path to progress IPC socket\n"
		"\t\t-h, --help              : print this help and exit\n"
		);
}

static void usage_dwlurl(const char *program) {
	fprintf(stdout,"\t %s \n", program);
	fprintf(stdout,
		"\t\t-u, --url <url>         : URL to be passed to the downloader\n"
		"\t\t-c, --userpassword user:pass : user / password to be used to download\n"
		"\t\t-h, --help              : print this help and exit\n"
		);
}

/*
 * Utility functions called by subcommands
 */
static bool check_ascii_char(const char *s) {
	int i;

	if (!s)
		return false;
	for (i = 0; i < strlen(s); i++) {
		if ((s[i] >= '0' && s[i] <= '9') ||
			(s[i] >= 'A' && s[i] <= 'F'))
			continue;
		return false;
	}

	return true;
}

static void send_msg(ipc_message *msg)
{
	int rc;

	fprintf(stdout, "Sending: '%s'", msg->data.procmsg.buf);
	rc = ipc_send_cmd(msg);

	fprintf(stdout, " returned %d\n", rc);
	if (rc == 0) {
		fprintf(stdout, "Server returns %s\n",
				(msg->type == ACK) ? "ACK" : "NACK");
		if (msg->data.procmsg.len > 0) {
			fprintf(stdout, "Returned message: %s\n",
					msg->data.procmsg.buf);
		}
	}
}

/*
 * Implementation of single commands
 */

static struct option hawkbitcfg_options[] = {
	{"help", no_argument, NULL, 'h'},
	{"polling-time", required_argument, NULL, 'p'},
	{"enable", no_argument, NULL, 'e'},
	{"disable", no_argument, NULL, 'd'},
	{"trigger", no_argument, NULL, 't'},
	{NULL, 0, NULL, 0}
};

static int hawkbitcfg(cmd_t  __attribute__((__unused__)) *cmd, int argc, char *argv[]) {
	ipc_message msg;
	size_t size;
	char *buf;
	int c;
	unsigned long polling_time;
	bool enable = false;
	bool trigger = false;
	int opt_e = 0;
	int opt_p = 0;

	memset(&msg, 0, sizeof(msg));
	msg.data.procmsg.source = SOURCE_SURICATTA;
	msg.type = SWUPDATE_SUBPROCESS;

	size = sizeof(msg.data.procmsg.buf);
	buf = msg.data.procmsg.buf;

	/* Process options with getopt */
	while ((c = getopt_long(argc, argv, "p:edth",
				hawkbitcfg_options, NULL)) != EOF) {
		switch (c) {
		case 'p':
			opt_p = 1;
			msg.data.procmsg.cmd = CMD_CONFIG;
			polling_time = strtoul(optarg, NULL, 10);
			break;
		case 'e':
		case 'd':
		case 't':
			msg.data.procmsg.cmd = CMD_ENABLE;
			opt_e = 1;
			trigger = (c == 't');
			enable = (c == 'e') || trigger;
			break;
		}
	}

	/*
	 * Build a json string with the command line parameters
	 * do not check anything, let SWUpdate
	 * doing the checks
	 * An error or a NACK is returned in
	 * case of failure
	 */
	if (opt_p) {
		snprintf(buf, size, "{ \"polling\" : \"%lu\"}", polling_time);
		msg.data.procmsg.len = strnlen(buf, size);
		send_msg(&msg);
	}
	if (opt_e) {
		snprintf(buf, size, trigger ? "{ \"trigger\" : %s}" : "{ \"enable\" : %s}", enable ? "true" : "false");
		msg.data.procmsg.len = strnlen(buf, size);
		send_msg(&msg);
	}

	exit(0);
}

static struct option dwlurl_options[] = {
	{"help", no_argument, NULL, 'h'},
	{"url", required_argument, NULL, 'u'},
	{"userpassword", required_argument, NULL, 'c'},
	{NULL, 0, NULL, 0}
};


static int dwlurl(cmd_t  __attribute__((__unused__)) *cmd, int argc, char *argv[]) {
	ipc_message msg;
	size_t size, len;
	char *buf;
	int c;
	int opt_u = 0, opt_c = 0;
	char *url = NULL, *user = NULL;

	memset(&msg, 0, sizeof(msg));
	msg.data.procmsg.source = SOURCE_DOWNLOADER;
	msg.type = SWUPDATE_SUBPROCESS;
	msg.data.procmsg.cmd = CMD_SET_DOWNLOAD_URL;

	size = sizeof(msg.data.procmsg.buf);
	buf = msg.data.procmsg.buf;

	/* Process options with getopt */
	while ((c = getopt_long(argc, argv, "u:c:",
				dwlurl_options, NULL)) != EOF) {
		switch (c) {
		case 'u':
			opt_u = 1;
			if (url) free(url);
			url = strdup(optarg);
			break;
		case 'c':
			opt_c = 1;
			if (user) free(user);
			user = strdup(optarg);
			break;
		}
	}

	/*
	 * Build a json string with the command line parameters
	 * do not check anything, let SWUpdate
	 * doing the checks
	 * An error or a NACK is returned in
	 * case of failure
	 */
	if (!opt_u) { /*this is mandatory */
		fprintf(stderr, "url is mandatory, skipping..\n");
		exit(1);
	}
	len = snprintf(buf, size, "{ \"url\": \"%s\"", url);
	if (len == size) {
		fprintf(stderr, "URL is too long : %s\n", url);
		exit(1);
	}
	if (opt_c) {
		len += snprintf(buf + len, size - len, ", \"userpassword\" : \"%s\" }",
				user);
	} else {
		len += snprintf(buf + len, size - len, "}");
	}
	if (len == size) {
		fprintf(stderr, "URL + credentials too long, not supported\n");
		exit(1);
	}
	msg.data.procmsg.len = len;
	send_msg(&msg);

	exit(0);
}

static int sendtohawkbit(cmd_t *cmd, int argc, char *argv[]) {
	int written, i;
	ipc_message msg;
	size_t size;
	char *buf;

	if (argc < 3) {
		cmd->usage(argv[0]);
		return 1;
	}

	memset(&msg, 0, sizeof(msg));
	msg.data.procmsg.source = SOURCE_SURICATTA;
	msg.data.procmsg.cmd = CMD_ACTIVATION;
	msg.type = SWUPDATE_SUBPROCESS;

	size = sizeof(msg.data.procmsg.buf);
	buf = msg.data.procmsg.buf;

	/*
	 * Build a json string with the command line parameters
	 * do not check anything, let SWUpdate
	 * doing the checks
	 * An error or a NACK is returned in
	 * case of failure
	 */
	for (i = 1; i < argc; i++) {
		switch (i) {
		case 1:
			written = snprintf(buf, size, "{ \"id\" : \"%lu\"", strtoul(argv[i], NULL, 10));
			break;
		case 2:
			written = snprintf(buf, size, ", \"status\" : \"%s\"", argv[i]);
			break;
		case 3:
			written = snprintf(buf, size, ",\"finished\" : \"%s\"", argv[i]);
			break;
		case 4:
			written = snprintf(buf, size, ",\"execution\" : \"%s\"", argv[i]);
			break;
		case 5:
			written = snprintf(buf, size, ",\"details\" : [ \"%s\"", argv[i]);
			break;
		default:
			written = snprintf(buf, size, ",\"%s\"", argv[i]);
			break;
		}

		buf += written;
		size -= written;

		if (size <= 0)
			break;
	}

	if (i > 5)
		written = snprintf(buf, size, "]}");
	else
		written = snprintf(buf, size, "}");

	fprintf(stdout, "Sending: '%s'", msg.data.procmsg.buf);
	msg.data.procmsg.len = strnlen(msg.data.procmsg.buf, sizeof(msg.data.procmsg.buf));

	send_msg(&msg);

	return 0;
}

static int gethawkbitstatus(cmd_t  __attribute__((__unused__)) *cmd,
			    int  __attribute__((__unused__)) argc,
			    char  __attribute__((__unused__)) *argv[]) {
	ipc_message msg;
	struct json_object *parsed_json;
	struct json_object *server;
	struct json_object *status;
	struct json_object *time;

	msg.type = SWUPDATE_SUBPROCESS;
	msg.data.procmsg.source = SOURCE_SURICATTA;
	msg.data.procmsg.cmd = CMD_GET_STATUS;

	msg.data.procmsg.buf[0] = '\0';
	msg.data.procmsg.len = 0;
	msg.data.procmsg.timeout = 10; /* Wait 10 s for Suricatta response */

	send_msg(&msg);

	if (msg.type == ACK) {
		parsed_json = json_tokener_parse(msg.data.procmsg.buf);
		json_object_object_get_ex(parsed_json, "server", &server);
		json_object_object_get_ex(server, "status", &status);
		json_object_object_get_ex(server, "time", &time);

		fprintf(stdout, "status: %d, time: %s\n",
		       json_object_get_int(status),
		       json_object_get_string(time));
		return 0;
	} else {
		fprintf(stderr, "Error: suricatta did respond with NACK.\n");
		return 1;
	}

}

static int sendaes(cmd_t *cmd, int argc, char *argv[]) {
	char *key, *ivt;
	if (argc != 3) {
		cmd->usage(argv[0]);
		return 1;
	}
	key = argv[1];
	ivt = argv[2];
	if (strlen(key) != 64 || strlen(ivt) != 32) {
		fprintf(stderr, "Wrong format for AES /IVT\n");
		cmd->usage(argv[0]);
		return 1;
	}
	if (!check_ascii_char(key) || !check_ascii_char(ivt)) {
		fprintf(stderr, "Wrong chars in keys\n");
		return 1;
	}
	if (swupdate_set_aes(key, ivt)) {
		fprintf(stderr, "Error setting AES KEY\n");
		return 1;
	}

	return 0;
}

static int setversions(cmd_t *cmd, int argc, char *argv[]) {
	if (argc != 4) {
		cmd->usage(argv[0]);
		return 1;
	}

	if (swupdate_set_version_range(argv[2], argv[3], argv[4])) {
		fprintf(stderr, "Error IPC setting versions\n");
		return 1;
	}
	return 0;
}

#if defined(CONFIG_CURL)
#include <curl/curl.h>

#define MAX_DEVS 100
#define PATTERN "REMOTE:"


/* Store the ip addresses of device to be rebooted */
static char ipaddrs[MAX_DEVS][NI_MAXHOST];

static bool is_ipaddress(char *ipaddr)
{
    struct sockaddr_in sa;
    int result = inet_pton(AF_INET, ipaddr, &(sa.sin_addr));
    return result == 1;
}

static struct option long_options[] = {
	{"help", no_argument, NULL, 'h'},
	{"wait", no_argument, NULL, 'w'},
	{"socket", required_argument, NULL, 's'},
	{NULL, 0, NULL, 0}
};

static void usage(char *programname)
{
	fprintf(stdout, "%s (compiled %s)\n", programname, __DATE__);
	fprintf(stdout, "Usage %s [OPTION]\n",
			programname);
	fprintf(stdout,
		" -w, --wait              : wait for a connection with SWUpdate\n"
		" -s, --socket <path>     : path to progress IPC socket\n"
		" -h, --help              : print this help and exit\n"
		);
}

static void restart_system(unsigned int ndevs)
{
	int dev;
	CURL *curl_handle;	/* CURL handle */
	char url[NI_MAXHOST + 20];
	CURLcode curlrc;
	struct ifaddrs *ifaddr, *ifa;
	char local[NI_MAXHOST];

	/*
	 * Drop local ip address from the list to avoid that
	 * this board reboots before sending all messages
	 * A local reboot will be done by calling reboot()
	 */
	if (getifaddrs(&ifaddr) != -1) {
		for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
			if ((ifa->ifa_addr == NULL) || (ifa->ifa_addr->sa_family != AF_INET))
				continue;
			if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
				local, NI_MAXHOST, NULL, 0, NI_NUMERICHOST))
					continue;

				for (dev = 0; dev < ndevs; dev++) {
					if (!strcmp(local, ipaddrs[dev])) {
						printf("LOCAL IP : %s %s\n", local, ipaddrs[dev]);
						ipaddrs[dev][0] = '\0';
					}
				}
		}
	}

	freeifaddrs(ifaddr);

	for (dev = 0; dev < ndevs; dev++) {
		if (!strlen(ipaddrs[dev]))
			continue;

		curl_handle = curl_easy_init();
		/* something very bad, it should never happen */
		if (!curl_handle)
			exit(2);
		snprintf(url, sizeof(url), "http://%s:8080/restart", ipaddrs[dev]);
		if ((curl_easy_setopt(curl_handle, CURLOPT_POST, 1L) != CURLE_OK) ||
			/* get verbose debug output please */
			(curl_easy_setopt(curl_handle, CURLOPT_VERBOSE,
				1L) != CURLE_OK) ||
			(curl_easy_setopt(curl_handle, CURLOPT_URL,
				url) != CURLE_OK) ||
			(curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS,
				"swupdate=reboot") != CURLE_OK) ||
			(curl_easy_setopt(curl_handle, CURLOPT_USERAGENT,
			"libcurl-agent/1.0") != CURLE_OK)) {
				fprintf(stderr, "Error setting curl options\n");
				exit(2);
		}

		fprintf(stdout, "Rebooting %s\n", url);

		curlrc = curl_easy_perform(curl_handle);
		if (curlrc != CURLE_OK && curlrc != CURLE_GOT_NOTHING) {
			fprintf(stderr, "Cannot reboot %s, try the next one, error(%d) : %s\n",
				ipaddrs[dev], curlrc, curl_easy_strerror(curlrc));
		}
		curl_easy_cleanup(curl_handle);
	}
}

static int sysrestart(cmd_t  __attribute__((__unused__)) *cmd, int argc, char *argv[]) {
	int connfd;
	struct progress_msg msg;
	int opt_w = 0;
	int c;
	int ret;
	int ndevs = 0;
	char *socket_path = NULL;

	RECOVERY_STATUS	status = IDLE;		/* Update Status (Running, Failure) */

	/* Process options with getopt */
	while ((c = getopt_long(argc, argv, "whs:",
				long_options, NULL)) != EOF) {
		switch (c) {
		case 'w':
			opt_w = 1;
			break;
		case 's':
			socket_path = strdup(optarg);
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
			break;
		default:
			usage(argv[0]);
			exit(1);
			break;
		}
	}

	/* initialize CURL */
	ret = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (ret != CURLE_OK) {
		fprintf(stderr, "CURL cannot be initialized, exiting..\n");
		exit(1);
	}

	connfd = -1;
	while (1) {
		if (connfd < 0) {
			if (!socket_path)
				connfd = progress_ipc_connect(opt_w);
			else
				connfd = progress_ipc_connect_with_path(socket_path, opt_w);
		}

		/*
		 * if still fails, try later
		 */
		if (connfd < 0) {
			sleep(1);
			continue;
		}

		if (progress_ipc_receive(&connfd, &msg) <= 0) {
			continue;
		}

		/*
		 * Something happens, show the info
		 */
		if ((status == IDLE) && (msg.status != IDLE)) {
			fprintf(stdout, "\nUpdate started !\n");
			fprintf(stdout, "Interface: ");
			switch (msg.source) {
			case SOURCE_UNKNOWN:
				fprintf(stdout, "UNKNOWN\n\n");
				break;
			case SOURCE_WEBSERVER:
				fprintf(stdout, "WEBSERVER\n\n");
				break;
			case SOURCE_SURICATTA:
				fprintf(stdout, "BACKEND\n\n");
				break;
			case SOURCE_DOWNLOADER:
				fprintf(stdout, "DOWNLOADER\n\n");
				break;
			case SOURCE_LOCAL:
				fprintf(stdout, "LOCAL\n\n");
				break;
			default:
				break;
			}
		}

		if (msg.infolen > 0) {
			/*
			 * check that msg is NULL terminated
			 */
			if (msg.infolen > sizeof(msg.info) - 1) {
				msg.infolen = sizeof(msg.info) - 1;
			}
			msg.info[msg.infolen] = '\0';
			char *ipaddr = strstr(msg.info, PATTERN);
			char *end;
			if (ipaddr && (strlen(ipaddr) > strlen(PATTERN))) {
				ipaddr += strlen(PATTERN);
				end = strchr(ipaddr, '}');
				if (end)
					*end = '\0';
				if (is_ipaddress(ipaddr)) {
					memset(ipaddrs[ndevs], 0, NI_MAXHOST);
					strncpy(ipaddrs[ndevs], ipaddr, sizeof(ipaddrs[ndevs]));
					fprintf(stdout, "Remote device:%s\n", ipaddr);
					ndevs++;
				}
			} else
				fprintf(stdout, "INFO : %s\n", msg.info);
		}

		switch (msg.status) {
		case SUCCESS:
			fprintf(stdout, "Ready to reboot !\n");
			restart_system(ndevs);
			sleep(5);
			sync();
			if (reboot(RB_AUTOBOOT) < 0) { /* It should never happen */
				fprintf(stdout, "Please reset the board.\n");
			}
			break;
		case FAILURE:
			ndevs = 0;
			break;
		case DONE:
			fprintf(stdout, "\nDONE.\n");
			break;
		default:
			break;
		}
		status = msg.status;
	}
}
#else
static int sysrestart(cmd_t __attribute__((__unused__)) *cmd,
		      int __attribute__((__unused__)) argc,
		      char **argv) {
	fprintf(stderr, "%s: Curl not available, exiting..\n", argv[1]);
	return 1;
}
#endif

static struct option monitor_options[] = {
	{"help", no_argument, NULL, 'h'},
	{"socket", required_argument, NULL, 's'},
	{NULL, 0, NULL, 0}
};

static int monitor(cmd_t  __attribute__((__unused__)) *cmd, int argc, char *argv[]) {
	char *socket_path = NULL;

	/* Process options with getopt */
	int c;
	while ((c = getopt_long(argc, argv, "hs:", monitor_options, NULL)) != EOF) {
		switch (c) {
		case 's':
			free(socket_path);
			socket_path = strdup(optarg);
			break;
		case 'h':
			usage_monitor(argv[0]);
			exit(0);
			break;
		default:
			usage_monitor(argv[0]);
			exit(1);
			break;
		}
	}

	int connfd = -1;
	struct progress_msg msg;
	while (1) {
		if (connfd < 0) {
			if (!socket_path)
				connfd = progress_ipc_connect(true);
			else
				connfd = progress_ipc_connect_with_path(socket_path, true);
		}

		/*
		 * if still fails, try later
		 */
		if (connfd < 0) {
			sleep(1);
			continue;
		}

		if (progress_ipc_receive(&connfd, &msg) <= 0) {
			continue;
		}

		if (msg.infolen > 0) {
			/*
			 * check that msg is NULL terminated
			 */
			if (msg.infolen > sizeof(msg.info) - 1) {
				msg.infolen = sizeof(msg.info) - 1;
			}
			msg.info[msg.infolen] = '\0';
		}

		/*
		 * ensure strings are null-terminated (they usually are by construction)
		 */
		msg.hnd_name[sizeof(msg.hnd_name) - 1] = '\0';
		msg.cur_image[sizeof(msg.cur_image) - 1] = '\0';

		fprintf(stdout, "[{ \"apiversion\": 0x%x, \"status\": %u, \"dwl_percent\": %u, \"dwl_bytes\": %llu"
				", \"nsteps\": %u, \"cur_step\": %u, \"cur_percent\": %u, \"cur_image\": \"%s\""
				", \"hnd_name\": \"%s\", \"source\": %u, \"infolen\": %u }",
				msg.apiversion, msg.status, msg.dwl_percent,
				msg.dwl_bytes, msg.nsteps, msg.cur_step,
				msg.cur_percent, msg.cur_image, msg.hnd_name,
				msg.source, msg.infolen);
                if (msg.infolen > 0) fprintf(stdout, ", %s]\n", msg.info); else fprintf(stdout, "]\n");

		fflush(stdout);
	}
	return 0;
}

/*
 * List of implemented commands
 */
cmd_t commands[] = {
	{"aes", sendaes, usage_aes},
	{"setversion",setversions, usage_setversion},
	{"sendtohawkbit", sendtohawkbit, usage_send_to_hawkbit},
	{"hawkbitcfg", hawkbitcfg, usage_hawkbitcfg},
	{"gethawkbit", gethawkbitstatus, usage_gethawkbitstatus},
	{"sysrestart", sysrestart, usage_sysrestart},
	{"monitor", monitor, usage_monitor},
	{"dwlurl", dwlurl, usage_dwlurl},
	{NULL, NULL, NULL}
};

/*
 * General help
 */
static void main_usage(char *program) {
	cmd_t *cmd;

	fprintf(stdout, "%s COMMAND [OPTIONS]\n", program);
	for (cmd = commands; cmd->name != NULL; cmd ++) {
		cmd->usage(cmd->name);
	}

	exit(0);
}

/*
 * main loop, iterates to find the command to be executed
 */
int main(int argc, char *argv[]) {
	cmd_t *cmd;
	if (argc < 2)
		main_usage(argv[0]);

	for (cmd = commands; cmd->name != NULL; cmd ++) {
		if (!strcmp(cmd->name, argv[1])) {
			return cmd->fn(cmd, argc - 1, &argv[1]);
		}
	}
	main_usage(argv[0]);

	exit(1);
}
