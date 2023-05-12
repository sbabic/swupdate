# SPDX-FileCopyrightText: 2021 Weidmueller Interface GmbH & Co. KG
#
# SPDX-License-Identifier: GPL-2.0-only

lib-$(CONFIG_DISKFORMAT) += diskformat.o
lib-$(CONFIG_FAT_FILESYSTEM) += diskio.o \
				fat_fs.o \
				ff.o
lib-$(CONFIG_EXT_FILESYSTEM) += mke2fs.o

#lib-$(CONFIG_BTRFS_FILESYSTEM) += btrfs/hash.o btrfs/crc32c.o btrfs/btrfs.o btrfs/common.o btrfs/hash.o btrfs/xxhash.o btrfs/blake2b-ref.o btrfs/sha224-256.o btrfs/inode-item.o
lib-$(CONFIG_BTRFS_FILESYSTEM) += btrfs.o
