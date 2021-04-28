/*
 * Copyright (C) 2021 Weidmueller Interface GmbH & Co. KG
 * Roland Gaudig <roland.gaudig@weidmueller.com>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/ioctl.h>

#include "swupdate.h"
#include "util.h"

#include "ff.h"
#include "diskio.h"


#define SECTOR_SIZE	512

static int file_descriptor = -1;


/*
 * Extension to FatFs library: fatfs_init associates the fatfs library with
 * a disk device file. It has to be called before using any FatFs API function.
 */
int fatfs_init(const char *device)
{
	if (file_descriptor >= 0) {
		ERROR("Called fatfs_init second time without fatfs_release");
		return -1;
	}

	if (!device) {
		ERROR("Device name is NULL pointer");
		return -1;
	}

	file_descriptor = open(device, O_RDWR);

	if (file_descriptor < 0) {
		ERROR("Device %s cannot be opened: %s", device, strerror(errno));
		return -1;
	}

	return 0;
}

/*
 * Extension to FatFs libary: fatfs_release closes a disk device.
 */
void fatfs_release(void)
{
	if (file_descriptor >= 0) {
		(void)close(file_descriptor);
		file_descriptor = -1;
	}
}

DSTATUS disk_status(BYTE pdrv)
{
	DSTATUS status = 0;
	(void)pdrv;

	if (file_descriptor < 0)
		status |= STA_NOINIT;

	return status;
}

DSTATUS disk_initialize(BYTE pdrv)
{
	return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	(void)pdrv;

	if (!buff)
		return RES_PARERR;

	if (disk_status(pdrv))
		return RES_NOTRDY;

	if (pread(file_descriptor, buff, count * SECTOR_SIZE, sector * SECTOR_SIZE) != count * SECTOR_SIZE)
		return RES_ERROR;

	return RES_OK;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
	(void)pdrv;

	if (!buff)
		return RES_PARERR;

	if (disk_status(pdrv))
		return RES_NOTRDY;

	if (pwrite(file_descriptor, buff, count * SECTOR_SIZE, sector * SECTOR_SIZE) != count * SECTOR_SIZE)
		return RES_ERROR;

	return RES_OK;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	(void) pdrv;

	if (disk_status(pdrv))
		return RES_NOTRDY;

	switch (cmd) {
	case CTRL_SYNC:
		if (syncfs(file_descriptor) != 0)
			return RES_ERROR;
		break;
	case GET_SECTOR_COUNT:
	{
		off_t size = lseek(file_descriptor, 0, SEEK_END) / SECTOR_SIZE;

		if (!buff)
			return RES_PARERR;

		*(LBA_t *)buff = size;
		break;
	}
	case GET_SECTOR_SIZE:
		if (!buff)
			return RES_PARERR;

		*(WORD *)buff = SECTOR_SIZE;
		break;
	case GET_BLOCK_SIZE:
		/* Get erase block size of flash memories, return 1 if not a
		 * Flash memory or unknown.
		 */
		if (!buff)
			return RES_PARERR;

		*(WORD *)buff = 1;
		break;
	default:
		ERROR("cmd %d not implemented", cmd);
		return RES_PARERR;
		break;
	}

	return RES_OK;
}

DWORD get_fattime(void)
{
	time_t unix_time = time(NULL);
	struct tm *t = gmtime(&unix_time);

	/* FatFs times are based on year 1980 */
	DWORD tdos = ((t->tm_year - 80) & 0x3F) << 25;
	/* FatFs months start with 1 */
	tdos |= (t->tm_mon + 1) << 21;
	tdos |= t->tm_mday << 16;
	tdos |= t->tm_hour << 11;
	tdos |= t->tm_min << 5;
	/* Don't know how FatFs copes with leap seconds, therefore limit them */
	tdos |= (t->tm_sec % 60) / 2;

	return tdos;
}
