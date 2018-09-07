/*
 * Author: Maciej Pijanowski maciej.pijanowski@3mdeb.com
 * Copyright (C) 2017, 3mdeb
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include "bootloader.h"
#include "grub.h"


/* read environment from storage into RAM */
static int grubenv_open(struct grubenv_t *grubenv)
{
	FILE *fp = NULL;
	size_t size;
	int ret = 0;
	char *buf = NULL, *key = NULL, *value = NULL;

	fp = fopen(GRUBENV_PATH, "rb");
	if (!fp) {
		ERROR("Failed to open grubenv file: %s", GRUBENV_PATH);
		ret = -1;
		goto cleanup;
	}

	if (fseek(fp, 0, SEEK_END)) {
		ERROR("Failed to seek end grubenv file: %s", GRUBENV_PATH);
		ret = -1;
		goto cleanup;
	}

	size = (size_t)ftell(fp);

	if (size != GRUBENV_SIZE) {
		ERROR("Ivalid grubenv file size: %d", (int)size);
		ret = -1;
		goto cleanup;
	}

	if (fseek(fp, 0, SEEK_SET)) {
		ERROR("Failed to seek set grubenv file: %s", GRUBENV_PATH);
		ret = -1;
		goto cleanup;
	}

	buf = malloc(size);
	if (!buf) {
		ERROR("Not enough memory for environment");
		fclose(fp);
		ret = -ENOMEM;
		goto cleanup;
	}

	if (fread(buf, 1, size, fp) != size) {
		ERROR("Failed to read file %s", GRUBENV_PATH);
		ret = 1;
		goto cleanup;
	}

	if (memcmp(buf, GRUBENV_HEADER, strlen(GRUBENV_HEADER) -1)) {
		ERROR("Invalid grubenv header");
		ret = -1;
		goto cleanup;
	}

	/* truncate header, prepare buf for further splitting */
	if (!(strtok(buf, "\n"))) {
		ERROR("grubenv header not found");
		ret = -1;
		goto cleanup;
	}

	/* load key - value pairs from buffer into dictionary list */
	/* Following approach fails if grubenv block contains empty variable,
	 * such as `var=`. GRUB env tool allows this situation to happen so it
	 * should probably be reconsidered. dict_set_value seems to cannot
	 * assign NULL to a variable, which complicates things a little bit?
	 */
	do {
		key = strtok(NULL, "=");
		value = strtok(NULL, "\n");
		/* value is null if we are at the last line (# characters) */
		if (!value || !key)
			continue;
		ret = dict_set_value(&grubenv->vars, key, value);
		if (ret) {
			ERROR("Adding pair [%s] = %s into dictionary list"
				"failed\n", key, value);
			return ret;
		}
	} while (value && key);

cleanup:
	if (fp) fclose(fp);
	/* free(null) should not harm anything */
	free(buf);
	return ret;
}

static int grubenv_parse_script(struct grubenv_t *grubenv, const char *script)
{
	FILE *fp = NULL;
	int ret = 0;
	char *line = NULL, *key = NULL, *value = NULL;
	size_t len = 0;

	/* open script generated during sw-description parsing */
	fp = fopen(script, "rb");
	if (!fp) {
		ERROR("Failed to open grubenv script file: %s", script);
		ret = -1;
		goto cleanup;
	}

	/* load  key-value pairs from script into grubenv dictionary */
	/* Note that variables with no value assigned are skipped now.
	 * We should consider whether we want to replicate U-Boot behavior
	 * (unset if no value given). GRUB env tool distinguishes unsetting
	 * (removing) variable from environment and setting variable to an
	 * empty string (NULL) as two actions. We should think about if it
	 * turns out to be desired
	 */
	while ((getline(&line, &len, fp)) != -1) {
		key = strtok(line, " \t\n");
		value = strtok(NULL, "\t\n");
		if (value != NULL && key != NULL) {
			ret = dict_set_value(&grubenv->vars, key, value);
			if (ret) {
				ERROR("Adding pair [%s] = %s into dictionary"
					"list failed\n", key, value);
				goto cleanup;
			}
		}
	}

cleanup:
	if (fp) fclose(fp);
	/* free(null) should not harm anything */
	free(line);
	return ret;
}

/* I'm not sure about size member of grubenv_t struct
 * It seems to me that it is enough if we check size of env stored in dict list
 * only once, just before writing into grubenv file.
 */
static inline void grubenv_update_size(struct grubenv_t *grubenv)
{
	/* size in bytes of grubenv-formatted string */
	int size = 0;
	struct dict_entry *grubvar;

	/* lengths of strings + '=' and '\n' characters */
	LIST_FOREACH(grubvar, &grubenv->vars, next) {
		char *key = dict_entry_get_key(grubvar);
		char *value = dict_entry_get_value(grubvar);

		size = size + strlen(key) + strlen(value) + 2;
	}
	size += strlen(GRUBENV_HEADER);
	grubenv->size = size;
}

static int grubenv_write(struct grubenv_t *grubenv)
{
	FILE *fp = NULL;
	char *buf = NULL, *ptr, line[SWUPDATE_GENERAL_STRING_SIZE];
	struct dict_entry *grubvar;
	int ret = 0, llen = 0;

	grubenv_update_size(grubenv);
	if (grubenv->size > GRUBENV_SIZE) {
		ERROR("Not enough free space in envblk file, %ld",
			grubenv->size);
		ret = -1;
		goto cleanup;
	}

	fp = fopen(GRUBENV_PATH_NEW, "wb");
	if (!fp) {
		ERROR("Failed to open file: %s", GRUBENV_PATH_NEW);
		ret = -1;
		goto cleanup;
	}

	buf = malloc(GRUBENV_SIZE);
	if (!buf) {
		ERROR("Not enough memory for environment");
		ret = -ENOMEM;
		goto cleanup;
	}

	/* form grubenv-formatted block inside memory */
	/* +1 for null termination */
	strncpy(buf, GRUBENV_HEADER, strlen(GRUBENV_HEADER) + 1);

	LIST_FOREACH(grubvar, &grubenv->vars, next) {
		char *key = dict_entry_get_key(grubvar);
		char *value = dict_entry_get_value(grubvar);

		llen = strlen(key) + strlen(value) + 2;
		/* +1 for null termination */
		snprintf(line, llen + 1, "%s=%s\n", key, value);
		strncat(buf, line, llen);
	}

	/* # chars starts there */
	ptr = buf + grubenv->size;

	/* fill with '#' from current ptr position up to the end of block */
	memset(ptr, '#', buf + GRUBENV_SIZE - ptr);

	/* write buffer into grubenv.nev file */
	ret = fwrite(buf , 1, GRUBENV_SIZE, fp);
	if (ret != GRUBENV_SIZE) {
		ERROR("Failed to write file: %s. Bytes written: %d",
			GRUBENV_PATH_NEW, ret);
		ret = -1;
		goto cleanup;
	}

	/* rename grubenv.new into grubenv */
	if (rename(GRUBENV_PATH_NEW, GRUBENV_PATH)) {
		ERROR("Failed to move environment: %s into %s",
			GRUBENV_PATH_NEW, GRUBENV_PATH);
		ret = -1;
		goto cleanup;
	}

	ret = 0;

cleanup:
	if (fp) fclose(fp);
	/* free(null) should not harm anything */
	free(buf);
	return ret;
}

/* I'm not sure what would be the proper method to free memory from dict list
 * allocation */
static inline void grubenv_close(struct grubenv_t *grubenv)
{
	dict_drop_db(&grubenv->vars);
}

/* I feel that '#' and '=' characters should be forbidden. Although it's not
 * explicitly mentioned in original grub env code, they may cause unexpected
 * behavior */
int bootloader_env_set(const char *name, const char *value)
{
	static struct grubenv_t grubenv;
	int ret;

	/* read env into dictionary list in RAM */
	if ((ret = grubenv_open(&grubenv)))
		goto cleanup;

	/* set new variable or change value of existing one */
	if ((ret = dict_set_value(&grubenv.vars, (char *)name, (char *)value)))
		goto cleanup;

	/* form grubenv format out of dictionary list and save it to file */
	if ((ret = grubenv_write(&grubenv)))
		goto cleanup;

cleanup:
	grubenv_close(&grubenv);
	return ret;
}

int bootloader_env_unset(const char *name)
{
	static struct grubenv_t grubenv;
	int ret = 0;

	/* read env into dictionary list in RAM */
	if ((ret = grubenv_open(&grubenv)))
		goto cleanup;

	/* remove entry from dictionary list */
	dict_remove(&grubenv.vars, (char *)name);

	/* form grubenv format out of dictionary list and save it to file */
	if ((ret = grubenv_write(&grubenv)))
		goto cleanup;

cleanup:
	grubenv_close(&grubenv);
	return ret;
}

char *bootloader_env_get(const char *name)
{
	static struct grubenv_t grubenv;
	char *value = NULL, *var;
	int ret = 0;

	/* read env into dictionary list in RAM */
	if ((ret = grubenv_open(&grubenv)))
		goto cleanup;

	/* retrieve value of given variable from dictionary list */
	var = dict_get_value(&grubenv.vars, (char *)name);

	if (var)
		value = strdup(var);
cleanup:
	grubenv_close(&grubenv);
	return value;

}

int bootloader_apply_list(const char *script)
{
	static struct grubenv_t grubenv;
	int ret = 0;

	/* read env into dictionary list in RAM */
	if ((ret = grubenv_open(&grubenv)))
		goto cleanup;

	/* add variables from sw-description into dictionary list */
	if ((ret = grubenv_parse_script(&grubenv, script)))
		goto cleanup;

	/* form grubenv format out of dictionary list and save it to file */
	if ((ret = grubenv_write(&grubenv)))
		goto cleanup;

cleanup:
	grubenv_close(&grubenv);
	return ret;
}
