/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
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
#include <getopt.h>
#include <ifaddrs.h>
#include <netdb.h>

#if defined(CONFIG_CURL)
#include <curl/curl.h>
#include <progress_ipc.h>

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

int main(int argc, char **argv)
{
	int connfd;
	struct progress_msg msg;
	int opt_w = 0;
	int c;
	int ret;
	int ndevs = 0;

	RECOVERY_STATUS	status = IDLE;		/* Update Status (Running, Failure) */

	/* Process options with getopt */
	while ((c = getopt_long(argc, argv, "whs:",
				long_options, NULL)) != EOF) {
		switch (c) {
		case 'w':
			opt_w = 1;
			break;
		case 's':
			SOCKET_PROGRESS_PATH = strdup(optarg);
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
			connfd = progress_ipc_connect(opt_w);
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

			if (system("reboot") < 0) { /* It should never happen */
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
#warning "swupdate-sysrestart needs libcurl, replaced with dummy"

int main(int __attribute__((__unused__)) argc, char __attribute__((__unused__)) **argv)
{
	fprintf(stderr, "Curl not available, exiting..\n");
	exit(1);
}
#endif
