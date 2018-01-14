/*
 * (C) Copyright 2002-2008
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdint.h>

#define AES_KEY_LENGTH  (128 / 8)

struct env_opts {
        char *config_file;
        int aes_flag; /* Is AES encryption used? */
        uint8_t aes_key[AES_KEY_LENGTH];
};

extern struct env_opts *fw_env_opts;

int fw_parse_script(char *fname, struct env_opts *opts);
char *fw_getenv(char *name);
int fw_env_open(struct env_opts *opts);
int fw_env_write(char *name, char *value);
int fw_env_close(struct env_opts *opts);
int fw_env_flush(struct env_opts *opts);

extern unsigned	long  crc32	 (unsigned long, const unsigned char *, unsigned);
