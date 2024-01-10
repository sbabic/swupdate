/*
 * (C) Copyright 2024
 * Stefano Babic, stefano.babic@swupdate.org
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <linux/version.h>
#include <sys/ioctl.h>
#include <linux/major.h>
#include <linux/mmc/ioctl.h>
#include "emmc.h"
#include "util.h"

/*
 * Code taken from mmc-utils, mmc_cmds.c
 */
static int emmc_read_extcsd(int fd, __u8 *ext_csd)
{
	int ret = 0;
	struct mmc_ioc_cmd idata;
	memset(&idata, 0, sizeof(idata));
	memset(ext_csd, 0, sizeof(__u8) * 512);
	idata.write_flag = 0;
	idata.opcode = MMC_SEND_EXT_CSD;
	idata.arg = 0;
	idata.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;
	idata.blksz = 512;
	idata.blocks = 1;
	mmc_ioc_cmd_set_data(idata, ext_csd);

	ret = ioctl(fd, MMC_IOC_CMD, &idata);
	if (ret)
		ERROR("eMMC ioctl return error %d", ret);

	return ret;
}

static void fill_switch_cmd(struct mmc_ioc_cmd *cmd, __u8 index, __u8 value)
{
	cmd->opcode = MMC_SWITCH;
	cmd->write_flag = 1;
	cmd->arg = (MMC_SWITCH_MODE_WRITE_BYTE << 24) | (index << 16) |
		   (value << 8) | EXT_CSD_CMD_SET_NORMAL;
	cmd->flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;
}

static int emmc_write_extcsd_value(int fd, __u8 index, __u8 value, unsigned int timeout_ms)
{
	int ret = 0;
	struct mmc_ioc_cmd idata = {};

	fill_switch_cmd(&idata, index, value);

	/* Kernel will set cmd_timeout_ms if 0 is set */
	idata.cmd_timeout_ms = timeout_ms;

	ret = ioctl(fd, MMC_IOC_CMD, &idata);
	if (ret)
		ERROR("eMMC ioctl return error %d", ret);

	return ret;
} /* end of imported code */

int emmc_get_active_bootpart(int fd)
{
	int ret;
	uint8_t extcsd[512];
	int active;

	ret = emmc_read_extcsd(fd, extcsd);

	if (ret)
		return -1;

	/*
	 * Return partition number starting from 0
	 * This corresponds to mmcblkXboot0 and mmcblkXboot1
	 */
	active = ((extcsd[EXT_CSD_PART_CONFIG] & 0x38) >> 3) - 1;

	return active;
}

int emmc_write_bootpart(int fd, int bootpart)
{
	uint8_t value;
	int ret;
	uint8_t extcsd[512];

	/*
	 * Do not clear BOOT_ACK
	 */
	ret = emmc_read_extcsd(fd, extcsd);
	value = extcsd[EXT_CSD_PART_CONFIG] & (1 << 6);

	bootpart = ((bootpart + 1) & 0x3) << 3;
	value |= bootpart;

	ret = emmc_write_extcsd_value(fd, EXT_CSD_PART_CONFIG, value, 0);

	return ret;
}
