/*
 * (C) Copyright 2014
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <sys/types.h>
#include <stdio.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "swupdate.h"
#include "handler.h"
#include "util.h"
#include "pctl.h"

static void shell_handler(void);
static void shell_preinstall_handler(void);
static void shell_postinstall_handler(void);

static int execute_shell_script(struct img_type *img, const char *fnname)
{
	int ret;
	const char* tmp = get_tmpdirscripts();

	char shellscript[MAX_IMAGE_FNAME + strlen(tmp) +
        strlen("postinst") + 2];

	snprintf(shellscript, sizeof(shellscript),
		"%s%s", tmp, img->fname);
	if (chmod(shellscript, S_IRUSR | S_IWUSR | S_IXUSR)) {
		ERROR("Execution bit cannot be set for %s", shellscript);
		return -1;
	}
	snprintf(shellscript, sizeof(shellscript),
		 "%s%s %s %s", tmp, img->fname, fnname, img->type_data);

	ret = run_system_cmd(shellscript);

	return ret;
}

static int start_shell_script(struct img_type *img, void *data)
{
	const char *fnname;
	script_fn scriptfn;

	if (!data)
		return -EINVAL;

	scriptfn = *(script_fn *)data;

	switch (scriptfn) {
	case PREINSTALL:
		fnname="preinst";
		break;
	case POSTINSTALL:
		fnname="postinst";
		break;
	default:
		/* no error, simply no call */
		return 0;
	}

	return execute_shell_script(img, fnname);
}

static int start_preinstall_script(struct img_type *img, void *data)
{
	script_fn scriptfn;

	if (!data)
		return -EINVAL;

	scriptfn = *(script_fn *)data;

	/*
	 * Call only in case of preinstall
	 */
	if (scriptfn != PREINSTALL)
		return 0;

	return execute_shell_script(img, "");
}

static int start_postinstall_script(struct img_type *img, void *data)
{
	script_fn scriptfn;

	if (!data)
		return -EINVAL;

	scriptfn = *(script_fn *)data;

	/*
	 * Call only in case of postinstall
	 */
	if (scriptfn != POSTINSTALL)
		return 0;

	return execute_shell_script(img, "");
}

 __attribute__((constructor))
static void shell_handler(void)
{
	register_handler("shellscript", start_shell_script,
				SCRIPT_HANDLER, NULL);
}

 __attribute__((constructor))
static void shell_preinstall_handler(void)
{
	register_handler("preinstall", start_preinstall_script,
				SCRIPT_HANDLER, NULL);
}

 __attribute__((constructor))
static void shell_postinstall_handler(void)
{
	register_handler("postinstall", start_postinstall_script,
				SCRIPT_HANDLER, NULL);
}
