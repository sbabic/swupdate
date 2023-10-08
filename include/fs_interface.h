/*
 * Copyright (C) 2021 Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

char *diskformat_fs_detect(char *device);
int diskformat_fs_exists(char *device, char *fstype);

int diskformat_mkfs(char *device, char *fstype);

#if defined(CONFIG_FAT_FILESYSTEM)
extern int fat_mkfs(const char *device_name, const char *fstype);
#endif

#if defined (CONFIG_EXT_FILESYSTEM) 
extern int ext_mkfs(const char *device_name, const char *fstype, unsigned long features,
		const char *volume_label);
#endif

#if defined (CONFIG_BTRFS_FILESYSTEM) 
extern int btrfs_mkfs(const char *device_name, const char *fstype);
#endif
