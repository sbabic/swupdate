/*
 * (C) Copyright 2008-2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#ifndef _IPC_H
#define _IPC_H

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "swupdate_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Be careful to include further headers here. This file is the interface
 * to external programs interfacing with SWUpdate as client, and further
 * headers are not exported.
 */

#define IPC_MAGIC		0x14052001

typedef enum {
	REQ_INSTALL,
	ACK,
	NACK,
	GET_STATUS,
	POST_UPDATE,
	SWUPDATE_SUBPROCESS,
	SET_AES_KEY,
	SET_UPDATE_STATE,	/* set bootloader ustate */
	GET_UPDATE_STATE,
	REQ_INSTALL_EXT,
	SET_VERSIONS_RANGE
} msgtype;

/*
 * Commands are used for IPC to subprocesses. The meaning is then interpreted
 * by the single subprocess
 */
enum {
	CMD_ACTIVATION,	/* this returns the answer if a SW can be activated */
	CMD_CONFIG,
	CMD_ENABLE	/* Enable or disable suricatta mode */
};

enum run_type {
	RUN_DEFAULT,
	RUN_DRYRUN,
	RUN_INSTALL
};

#define SWUPDATE_API_VERSION 	0x1
/*
 * Install structure to be filled before calling
 * ipc and async functions
 */
struct swupdate_request {
	unsigned int apiversion;
	sourcetype source;
	enum run_type dry_run;
	size_t len;
	char info[512];
	char software_set[256];
	char running_mode[256];
	bool disable_store_swu;
};

typedef union {
	char msg[128];
	struct { 
		int current;
		int last_result;
		int error;
		char desc[2048];
	} status;
	struct {
		struct swupdate_request req;
		unsigned int len;    /* Len of data valid in buf */
		char	buf[2048];   /*
				      * Buffer that each source can fill
				      * with additional information
				      */
	} instmsg;
	struct {
		sourcetype source; /* Who triggered the update */
		int	cmd;	   /* Optional encoded command */
		int	timeout;     /* timeout in seconds if an aswer is expected */
		unsigned int len;    /* Len of data valid in buf */
		char	buf[2048];   /*
				      * Buffer that each source can fill
				      * with additional information
				      */
	} procmsg;
	struct {
		char key_ascii[65]; /* Key size in ASCII (256 bit, 32 bytes bin) + termination */
		char ivt_ascii[33]; /* Key size in ASCII (16 bytes bin) + termination */
	} aeskeymsg;
	struct {
		char minimum_version[256];
		char maximum_version[256];
		char current_version[256];
	} versions;
} msgdata;
	
typedef struct {
	int magic;	/* magic number */
	int type;
	msgdata data;
} ipc_message;

char *get_ctrl_socket(void);
int ipc_inst_start(void);
int ipc_inst_start_ext(void *priv, ssize_t size);
int ipc_send_data(int connfd, char *buf, int size);
void ipc_end(int connfd);
int ipc_get_status(ipc_message *msg);
int ipc_get_status_timeout(ipc_message *msg, unsigned int timeout_ms);
int ipc_postupdate(ipc_message *msg);
int ipc_send_cmd(ipc_message *msg);

typedef int (*writedata)(char **buf, int *size);
typedef int (*getstatus)(ipc_message *msg);
typedef int (*terminated)(RECOVERY_STATUS status);
int ipc_wait_for_complete(getstatus callback);
void swupdate_prepare_req(struct swupdate_request *req);
int swupdate_image_write(char *buf, int size);
int swupdate_async_start(writedata wr_func, getstatus status_func,
				terminated end_func,
				void *priv, ssize_t size);
int swupdate_set_aes(char *key, char *ivt);
int swupdate_set_version_range(const char *minversion,
				const char *maxversion,
				const char *currentversion);
#ifdef __cplusplus
}   // extern "C"
#endif

#endif
