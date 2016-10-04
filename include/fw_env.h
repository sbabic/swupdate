/*
 * (C) Copyright 2002-2008
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
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

extern unsigned	long  crc32	 (unsigned long, const unsigned char *, unsigned);
