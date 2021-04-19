/*
 * Copyright (C) 2021 Stefano Babic, sbabic@denx.de
 *
 * The code is mostly taken and modified from
 * mke2fs.c - Make a ext2fs filesystem.
 *
 * Copyright (C) 1994, 1995, 1996, 1997, 1998, 1999, 2000, 2001, 2002,
 * 	2003, 2004, 2005 by Theodore Ts'o.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * mke2fs uses the libe2fs library, but there is a lot of to do
 * to initialize a extX filesystem. mke2fs has a lot of options
 * and features that are not required by SWUpdate.
 * This is a port from the original mke2fs, with these goals:
 * - create a library to initialize a fs instead of a tool
 * - use just default options
 *
 * This library has the same functionality compared to call
 * mke2fs without option, that is:
 *
 * 	mke2fs -t <filesystem type> <device>
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <libgen.h>
#include <limits.h>
#include <blkid/blkid.h>
#include <uuid/uuid.h>
#include "util.h"
#include "fs_interface.h"

#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2fs.h>

#define STRIDE_LENGTH 8

#define MAX_32_NUM ((((unsigned long long) 1) << 32) - 1)

static __u64	offset;

static __u32 zero_buf[4];

static int int_log2(unsigned long long arg)
{
	int	l = 0;

	arg >>= 1;
	while (arg) {
		l++;
		arg >>= 1;
	}
	return l;
}

/*
 * Determine the number of journal blocks to use, either via
 * user-specified # of megabytes, or via some intelligently selected
 * defaults.
 *
 * Find a reasonable journal file size (in blocks) given the number of blocks
 * in the filesystem.  For very small filesystems, it is not reasonable to
 * have a journal that fills more than half of the filesystem.
 */
static unsigned int figure_journal_size(int size, ext2_filsys fs)
{
	int j_blocks;

	j_blocks = ext2fs_default_journal_size(ext2fs_blocks_count(fs->super));
	if (j_blocks < 0) {
		WARN("Filesystem too small for a journal");
		return 0;
	}

	if (size > 0) {
		j_blocks = size * 1024 / (fs->blocksize	/ 1024);
		if (j_blocks < 1024 || j_blocks > 10240000) {
			ERROR("\nThe requested journal "
				"size is %d blocks; it must be\n"
				"between 1024 and 10240000 blocks.  "
				"Aborting.\n",
				j_blocks);
			return 0;
		}
		if ((unsigned) j_blocks > ext2fs_free_blocks_count(fs->super) / 2) {
			WARN("Journal size too big for filesystem.\n");
			return 0;
		}
	}
	return j_blocks;
}

static int write_reserved_inodes(ext2_filsys fs)
{
	errcode_t	retval;
	ext2_ino_t	ino;
	struct ext2_inode *inode;

	retval = ext2fs_get_memzero(EXT2_INODE_SIZE(fs->super), &inode);
	if (retval) {
		ERROR("inode_init while allocating memory: %ld", retval);
		return -ENOMEM;
	}

	for (ino = 1; ino < EXT2_FIRST_INO(fs->super); ino++) {
		retval = ext2fs_write_inode_full(fs, ino, inode,
						 EXT2_INODE_SIZE(fs->super));
		if (retval) {
			ERROR("ext2fs_write_inode_full while writing reserved inodes : %ld", retval);
			return -1;
		}
	}

	ext2fs_free_mem(&inode);

	return 0;
}

static errcode_t packed_allocate_tables(ext2_filsys fs)
{
	errcode_t	retval;
	dgrp_t		i;
	blk64_t		goal = 0;

	for (i = 0; i < fs->group_desc_count; i++) {
		retval = ext2fs_new_block2(fs, goal, NULL, &goal);
		if (retval)
			return retval;
		ext2fs_block_alloc_stats2(fs, goal, +1);
		ext2fs_block_bitmap_loc_set(fs, i, goal);
	}
	for (i = 0; i < fs->group_desc_count; i++) {
		retval = ext2fs_new_block2(fs, goal, NULL, &goal);
		if (retval)
			return retval;
		ext2fs_block_alloc_stats2(fs, goal, +1);
		ext2fs_inode_bitmap_loc_set(fs, i, goal);
	}
	for (i = 0; i < fs->group_desc_count; i++) {
		blk64_t end = ext2fs_blocks_count(fs->super) - 1;
		retval = ext2fs_get_free_blocks2(fs, goal, end,
						 fs->inode_blocks_per_group,
						 fs->block_map, &goal);
		if (retval)
			return retval;
		ext2fs_block_alloc_stats_range(fs, goal,
					       fs->inode_blocks_per_group, +1);
		ext2fs_inode_table_loc_set(fs, i, goal);
		ext2fs_group_desc_csum_set(fs, i);
	}
	return 0;
}

static int write_inode_tables(ext2_filsys fs, int lazy_flag, int itable_zeroed)
{
	errcode_t	retval;
	blk64_t		blk;
	dgrp_t		i;
	int		num;

	for (i = 0; i < fs->group_desc_count; i++) {

		blk = ext2fs_inode_table_loc(fs, i);
		num = fs->inode_blocks_per_group;

		if (lazy_flag)
			num = ext2fs_div_ceil((fs->super->s_inodes_per_group -
					       ext2fs_bg_itable_unused(fs, i)) *
					      EXT2_INODE_SIZE(fs->super),
					      EXT2_BLOCK_SIZE(fs->super));
		if (!lazy_flag || itable_zeroed) {
			/* The kernel doesn't need to zero the itable blocks */
			ext2fs_bg_flags_set(fs, i, EXT2_BG_INODE_ZEROED);
			ext2fs_group_desc_csum_set(fs, i);
		}
		if (!itable_zeroed) {
			retval = ext2fs_zero_blocks2(fs, blk, num, &blk, &num);
			if (retval) {
				ERROR("Could not write %d "
					  "blocks in inode table starting at %llu",
					num, blk);
				return -1;
			}
		}
	}

	/* Reserved inodes must always have correct checksums */
	if (ext2fs_has_feature_metadata_csum(fs->super))
		write_reserved_inodes(fs);

	return 0;
}

static int create_root_dir(ext2_filsys fs, uid_t root_uid, gid_t root_gid)
{
	errcode_t		retval;
	struct ext2_inode	inode;

	retval = ext2fs_mkdir(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, 0);
	if (retval) {
		ERROR("%s ext2fs_mkdir while creating root dir", strerror(retval));
		return -1;
	}
	if (root_uid != 0 || root_gid != 0) {
		retval = ext2fs_read_inode(fs, EXT2_ROOT_INO, &inode);
		if (retval) {
			ERROR("%s ext2fs_read_inode while reading root inode", strerror(retval));
			return -1;
		}

		inode.i_uid = root_uid;
		ext2fs_set_i_uid_high(inode, root_uid >> 16);
		inode.i_gid = root_gid;
		ext2fs_set_i_gid_high(inode, root_gid >> 16);

		retval = ext2fs_write_new_inode(fs, EXT2_ROOT_INO, &inode);
		if (retval) {
			ERROR("%s ext2fs_write_inode while setting root inode ownership", strerror(retval));
			return -1;
		}
	}

	return 0;
}

static int create_lost_and_found(ext2_filsys fs)
{
	unsigned int		lpf_size = 0;
	errcode_t		retval;
	ext2_ino_t		ino;
	const char		*name = "lost+found";
	int			i;

	fs->umask = 077;
	retval = ext2fs_mkdir(fs, EXT2_ROOT_INO, 0, name);
	if (retval) {
		ERROR("%s ext2fs_mkdir while creating /lost+found", strerror(retval));
		return -1;
	}

	retval = ext2fs_lookup(fs, EXT2_ROOT_INO, name, strlen(name), 0, &ino);
	if (retval) {
		ERROR("%s ext2_lookup while looking up /lost+found", strerror(retval));
		return -1;
	}

	for (i=1; i < EXT2_NDIR_BLOCKS; i++) {
		/* Ensure that lost+found is at least 2 blocks, so we always
		 * test large empty blocks for big-block filesystems.  */
		if ((lpf_size += fs->blocksize) >= 16*1024 &&
		    lpf_size >= 2 * fs->blocksize)
			break;
		retval = ext2fs_expand_dir(fs, ino);
		if (retval) {
			ERROR("%s ext2fs_expand_dir while expanding /lost+found", strerror(retval));
			return -1;
		}
	}

	return 0;
}

static int create_bad_block_inode(ext2_filsys fs, badblocks_list bb_list)
{
	errcode_t	retval;

	ext2fs_mark_inode_bitmap2(fs->inode_map, EXT2_BAD_INO);
	ext2fs_inode_alloc_stats2(fs, EXT2_BAD_INO, +1, 0);
	retval = ext2fs_update_bb_inode(fs, bb_list);
	if (retval) {
		ERROR("ext2fs_update_bb_inode %ld"
			"while setting bad block inode", retval);
		return -1;
	}

	return 0;
}

static void reserve_inodes(ext2_filsys fs)
{
	ext2_ino_t	i;

	for (i = EXT2_ROOT_INO + 1; i < EXT2_FIRST_INODE(fs->super); i++)
		ext2fs_inode_alloc_stats2(fs, i, +1, 0);
	ext2fs_mark_ib_dirty(fs);
}

#define BSD_DISKMAGIC   (0x82564557UL)  /* The disk magic number */
#define BSD_MAGICDISK   (0x57455682UL)  /* The disk magic number reversed */
#define BSD_LABEL_OFFSET        64

static int zap_sector(ext2_filsys fs, int sect, int nsect)
{
	char *buf;
	int retval;
	unsigned int *magic;

	buf = calloc(512, nsect);
	if (!buf) {
		ERROR("Out of memory erasing sectors %d-%d\n",
		       sect, sect + nsect - 1);
		return -ENOMEM;
	}

	if (sect == 0) {
		/* Check for a BSD disklabel, and don't erase it if so */
		retval = io_channel_read_blk64(fs->io, 0, -512, buf);
		if (retval)
			WARN("Warning: could not read block 0: %s\n",
				strerror(retval));
		else {
			magic = (unsigned int *) (buf + BSD_LABEL_OFFSET);
			if ((*magic == BSD_DISKMAGIC) ||
			    (*magic == BSD_MAGICDISK))
				return 0;
		}
	}

	memset(buf, 0, 512*nsect);
	io_channel_set_blksize(fs->io, 512);
	retval = io_channel_write_blk64(fs->io, sect, -512*nsect, buf);
	io_channel_set_blksize(fs->io, fs->blocksize);
	free(buf);
	if (retval)
		WARN("Warning: could not erase sector %d: %d",
			sect, retval);

	return 0;
}

/*
 * Sets the geometry of a device (stripe/stride), and returns the
 * device's alignment offset, if any, or a negative error.
 */
static int get_device_geometry(const char *file,
			       struct ext2_super_block *param,
			       unsigned int psector_size)
{
	int rc = -1;
	unsigned int blocksize;
	blkid_probe pr;
	blkid_topology tp;
	unsigned long min_io;
	unsigned long opt_io;
	struct stat statbuf;

	/* Nothing to do for a regular file */
	if (!stat(file, &statbuf) && S_ISREG(statbuf.st_mode))
		return 0;

	pr = blkid_new_probe_from_filename(file);
	if (!pr)
		goto out;

	tp = blkid_probe_get_topology(pr);
	if (!tp)
		goto out;

	min_io = blkid_topology_get_minimum_io_size(tp);
	opt_io = blkid_topology_get_optimal_io_size(tp);
	blocksize = EXT2_BLOCK_SIZE(param);
	if ((min_io == 0) && (psector_size > blocksize))
		min_io = psector_size;
	if ((opt_io == 0) && min_io)
		opt_io = min_io;
	if ((opt_io == 0) && (psector_size > blocksize))
		opt_io = psector_size;

	/* setting stripe/stride to blocksize is pointless */
	if (min_io > blocksize)
		param->s_raid_stride = min_io / blocksize;
	if (opt_io > blocksize)
		param->s_raid_stripe_width = opt_io / blocksize;

	rc = blkid_topology_get_alignment_offset(tp);
out:
	blkid_free_probe(pr);
	return rc;
}

static int mkfs_prepare(const char *device_name, struct ext2_super_block *pfs_param)
{
	int		cluster_size = 0;
	char 		*tmp;
	int		explicit_fssize = 0;
	int		blocksize = 0;
	int		inode_ratio = 0;
	int		inode_size = 0;
	unsigned long	flex_bg_size = 0;
	double		reserved_ratio = -1.0;
	int		lsector_size = 0, psector_size = 0;
	unsigned long long num_inodes = 0; /* unsigned long long to catch too-large input */
	errcode_t	retval;
	char *		usage_types = 0;
	int		num_backups = 2; /* number of backup bg's for sparse_super2 */
	blk64_t		dev_size;
	int 		sys_page_size = 4096;

	/*
	 * NOTE: A few words about fs_blocks_count and blocksize:
	 *
	 * Initially, blocksize is set to zero, which implies 1024.
	 * If -b is specified, blocksize is updated to the user's value.
	 *
	 * Next, the device size or the user's "blocks" command line argument
	 * is used to set fs_blocks_count; the units are blocksize.
	 *
	 * Later, if blocksize hasn't been set and the profile specifies a
	 * blocksize, then blocksize is updated and fs_blocks_count is scaled
	 * appropriately.  Note the change in units!
	 *
	 * Finally, we complain about fs_blocks_count > 2^32 on a non-64bit fs.
	 */
	blk64_t		fs_blocks_count = 0;
	int		use_bsize;

	pfs_param->s_rev_level = 1;  /* Create revision 1 filesystems now */

	/* Determine the size of the device (if possible) */
	retval = ext2fs_get_device_size2(device_name,
					 EXT2_BLOCK_SIZE(pfs_param),
					 &dev_size);
	if (retval && (retval != EXT2_ET_UNIMPLEMENTED)) {
		ERROR("%s while trying to determine filesystem size", strerror(retval));
		return -EINVAL;
	}

	if (retval == EXT2_ET_UNIMPLEMENTED) {
		ERROR("Couldn't determine device size; you "
			"must specify the size of the "
			"filesystem");
		return -EINVAL;
	} else {
		if (dev_size == 0) {
			ERROR("Device size reported to be zero.  "
			  "Invalid partition specified, or"
			  "partition table wasn't reread "
			  "after running fdisk, due to"
			  "a modified partition being busy "
			  "and in use.");
			ERROR("You may need to reboot"
			  "to re-read your partition table."
			  );
			return -EINVAL;
		}
		fs_blocks_count = dev_size;
		if (sys_page_size > EXT2_BLOCK_SIZE(pfs_param))
			fs_blocks_count &= ~((blk64_t) ((sys_page_size /
				     EXT2_BLOCK_SIZE(pfs_param))-1));
	}

	/*
	 * We have the file system (or device) size, so we can now
	 * determine the appropriate file system types so the fs can
	 * be appropriately configured.
	 */

	/* Get the hardware sector sizes, if available */
	retval = ext2fs_get_device_sectsize(device_name, &lsector_size);
	if (retval) {
		ERROR("%s while trying to determine hardware sector size", strerror(retval));
		return -EFAULT;
	}
	retval = ext2fs_get_device_phys_sectsize(device_name, &psector_size);
	if (retval) {
		ERROR("%s while trying to determine physical sector size", strerror(retval));
		return -EFAULT;
	}

	/* Older kernels may not have physical/logical distinction */
	if (!psector_size)
		psector_size = lsector_size;

	use_bsize = sys_page_size;
	if (lsector_size && use_bsize < lsector_size)
		use_bsize = lsector_size;
	if ((blocksize < 0) && (use_bsize < (-blocksize)))
		use_bsize = -blocksize;
	blocksize = use_bsize;
	fs_blocks_count /= (blocksize / 1024);


	pfs_param->s_log_block_size =
		int_log2(blocksize >> EXT2_MIN_BLOCK_LOG_SIZE);

	/*
	 * We now need to do a sanity check of fs_blocks_count for
	 * 32-bit vs 64-bit block number support.
	 */
	if ((fs_blocks_count > MAX_32_NUM) &&
	    ext2fs_has_feature_64bit(pfs_param))
		ext2fs_clear_feature_resize_inode(pfs_param);

	if ((fs_blocks_count > MAX_32_NUM) &&
	    !ext2fs_has_feature_64bit(pfs_param)) {
		ERROR("Size of device (0x%llx blocks) %s "
				  "too big to be expressed "
				  "in 32 bits using a blocksize of %d.",
			fs_blocks_count, device_name,
			EXT2_BLOCK_SIZE(pfs_param));
		return -EFAULT;
	}
	/*
	 * Guard against group descriptor count overflowing... Mostly to avoid
	 * strange results for absurdly large devices.  This is in log2:
	 * (blocksize) * (bits per byte) * (maximum number of block groups)
	 */
	if (fs_blocks_count >
	    (1ULL << (EXT2_BLOCK_SIZE_BITS(pfs_param) + 3 + 32)) - 1) {
		ERROR("Size of device (0x%llx blocks) %s "
				  "too big to create "
				  "a filesystem using a blocksize of %d.",
			fs_blocks_count, device_name,
			EXT2_BLOCK_SIZE(pfs_param));
		return -EFAULT;
	}

	ext2fs_blocks_count_set(pfs_param, fs_blocks_count);

	/* Get reserved_ratio from profile if not specified on cmd line. */
	reserved_ratio = 5.0;

	if (ext2fs_has_feature_journal_dev(pfs_param)) {
		reserved_ratio = 0;
		pfs_param->s_feature_incompat = EXT3_FEATURE_INCOMPAT_JOURNAL_DEV;
		pfs_param->s_feature_compat = 0;
		pfs_param->s_feature_ro_compat &=
					EXT4_FEATURE_RO_COMPAT_METADATA_CSUM;
 	}

	/* Check the user's mkfs options for 64bit */
	if (ext2fs_has_feature_64bit(pfs_param) &&
	    !ext2fs_has_feature_extents(pfs_param)) {
		ERROR("Extents MUST be enabled for a 64-bit "
			       "filesystem.  Pass -O extents to rectify.");
		return -EINVAL;
	}

	/* Set first meta blockgroup via an environment variable */
	/* (this is mostly for debugging purposes) */
	if (ext2fs_has_feature_meta_bg(pfs_param) &&
	    (tmp = getenv("MKE2FS_FIRST_META_BG")))
		pfs_param->s_first_meta_bg = atoi(tmp);
	if (ext2fs_has_feature_bigalloc(pfs_param)) {
		if (!cluster_size)
			cluster_size = blocksize*16;

		pfs_param->s_log_cluster_size =
			int_log2(cluster_size >> EXT2_MIN_CLUSTER_LOG_SIZE);
		if (pfs_param->s_log_cluster_size &&
		    pfs_param->s_log_cluster_size < pfs_param->s_log_block_size) {
			ERROR("The cluster size may not be "
				  "smaller than the block size.");
			return -EINVAL;
		}
	} else if (cluster_size) {
		ERROR("specifying a cluster size requires the "
			  "bigalloc feature");
		return -EINVAL;
	} else
		pfs_param->s_log_cluster_size = pfs_param->s_log_block_size;

	if (inode_ratio == 0) {
		inode_ratio = 8192;
		if (inode_ratio < blocksize)
			inode_ratio = blocksize;
		if (inode_ratio < EXT2_CLUSTER_SIZE(pfs_param))
			inode_ratio = EXT2_CLUSTER_SIZE(pfs_param);
	}

	retval = get_device_geometry(device_name, pfs_param,
				     (unsigned int) psector_size);
	if (retval < 0) {
		WARN("warning: Unable to get device geometry for %s\n",
			device_name);
	} else if (retval) {
		TRACE("%s alignment is offset by %lu bytes.",
		       device_name, retval);
		TRACE("This may result in very poor performance, "
			  "(re)-partitioning suggested.");
	}

	blocksize = EXT2_BLOCK_SIZE(pfs_param);

	/*
	 * Initialize s_desc_size so that the parse_extended_opts()
	 * can correctly handle "-E resize=NNN" if the 64-bit option
	 * is set.
	 */
	if (ext2fs_has_feature_64bit(pfs_param))
		pfs_param->s_desc_size = EXT2_MIN_DESC_SIZE_64BIT;

	/* This check should happen beyond the last assignment to blocksize */
	if (blocksize > sys_page_size) {
		WARN("Warning: %d-byte blocks too big for system "
				  "(max %d), forced to continue\n",
			blocksize, sys_page_size);
	}

	if (explicit_fssize == 0 && offset > 0) {
		fs_blocks_count -= offset / EXT2_BLOCK_SIZE(pfs_param);
		ext2fs_blocks_count_set(pfs_param, fs_blocks_count);
		WARN("Warning: offset specified without an "
			  "explicit file system size.\n"
			  "Creating a file system with %llu blocks "
			  "but this might\n"
			  "not be what you want.",
			(unsigned long long) fs_blocks_count);
	}

	if (ext2fs_has_feature_casefold(pfs_param) &&
	    ext2fs_has_feature_encrypt(pfs_param)) {
		ERROR("The encrypt and casefold features are not "
			  "compatible.\nThey can not be both enabled "
			  "simultaneously.");
		return -EINVAL;
	}

	/* Don't allow user to set both metadata_csum and uninit_bg bits. */
	if (ext2fs_has_feature_metadata_csum(pfs_param) &&
	    ext2fs_has_feature_gdt_csum(pfs_param))
		ext2fs_clear_feature_gdt_csum(pfs_param);

	/* Can't support bigalloc feature without extents feature */
	if (ext2fs_has_feature_bigalloc(pfs_param) &&
	    !ext2fs_has_feature_extents(pfs_param)) {
		ERROR("Can't support bigalloc feature without "
			  "extents feature");
		return -EINVAL;
	}

	if (ext2fs_has_feature_meta_bg(pfs_param) &&
	    ext2fs_has_feature_resize_inode(pfs_param)) {
		ERROR("The resize_inode and meta_bg "
					"features are not compatible.\n"
					"They can not be both enabled "
					"simultaneously.\n");
		return -EINVAL;
	}


	/*
	 * Since sparse_super is the default, we would only have a problem
	 * here if it was explicitly disabled.
	 */
	if (ext2fs_has_feature_resize_inode(pfs_param) &&
	    !ext2fs_has_feature_sparse_super(pfs_param)) {
		ERROR("reserved online resize blocks not supported "
			  "on non-sparse filesystem");
		return -EINVAL;
	}

	if (pfs_param->s_blocks_per_group) {
		if (pfs_param->s_blocks_per_group < 256 ||
		    pfs_param->s_blocks_per_group > 8 * (unsigned) blocksize) {
			ERROR("blocks per group count out of range");
			return -EINVAL;
		}
	}

	/*
	 * If the bigalloc feature is enabled, then the -g option will
	 * specify the number of clusters per group.
	 */
	if (ext2fs_has_feature_bigalloc(pfs_param)) {
		pfs_param->s_clusters_per_group = pfs_param->s_blocks_per_group;
		pfs_param->s_blocks_per_group = 0;
	}

	if (inode_size == 0)
		inode_size = 256;
	if (!flex_bg_size && ext2fs_has_feature_flex_bg(pfs_param))
		flex_bg_size = 16;
	if (flex_bg_size) {
		if (!ext2fs_has_feature_flex_bg(pfs_param)) {
			ERROR("Flex_bg feature not enabled, so "
				  "flex_bg size may not be specified");
			return -EINVAL;
		}
		pfs_param->s_log_groups_per_flex = int_log2(flex_bg_size);
	}

	if (inode_size && pfs_param->s_rev_level >= EXT2_DYNAMIC_REV) {
		if (inode_size < EXT2_GOOD_OLD_INODE_SIZE ||
		    inode_size > EXT2_BLOCK_SIZE(pfs_param) ||
		    inode_size & (inode_size - 1)) {
			ERROR("invalid inode size %d (min %d/max %d)",
				inode_size, EXT2_GOOD_OLD_INODE_SIZE,
				blocksize);
			return -EINVAL;
		}
		pfs_param->s_inode_size = inode_size;
	}

	/*
	 * If inode size is 128 and inline data is enabled, we need
	 * to notify users that inline data will never be useful.
	 */
	if (ext2fs_has_feature_inline_data(pfs_param) &&
	    pfs_param->s_inode_size == EXT2_GOOD_OLD_INODE_SIZE) {
		ERROR("%d byte inodes are too small for inline data; "
			  "specify larger size",
			pfs_param->s_inode_size);
		return -EINVAL;
	}

	/* Make sure number of inodes specified will fit in 32 bits */
	unsigned long long n;
	n = ext2fs_blocks_count(pfs_param) * blocksize / inode_ratio;
	if (n > MAX_32_NUM) {
		if (ext2fs_has_feature_64bit(pfs_param))
			num_inodes = MAX_32_NUM;
		else {
			ERROR("too many inodes (%llu), raise "
				  "inode ratio?", n);
			return -EINVAL;
		}
	}

	/*
	 * Calculate number of inodes based on the inode ratio
	 */
	pfs_param->s_inodes_count = num_inodes ? num_inodes :
		(ext2fs_blocks_count(pfs_param) * blocksize) / inode_ratio;

	if ((((unsigned long long)pfs_param->s_inodes_count) *
	     (inode_size ? inode_size : EXT2_GOOD_OLD_INODE_SIZE)) >=
	    ((ext2fs_blocks_count(pfs_param)) *
	     EXT2_BLOCK_SIZE(pfs_param))) {
		ERROR("inode_size (%u) * inodes_count "
					  "(%u) too big for a\n\t"
					  "filesystem with %llu blocks, "
					  "specify higher inode_ratio (-i)\n\t"
					  "or lower inode count (-N).\n",
			inode_size ? inode_size : EXT2_GOOD_OLD_INODE_SIZE,
			pfs_param->s_inodes_count,
			(unsigned long long) ext2fs_blocks_count(pfs_param));
		return -EINVAL;
	}

	/*
	 * Calculate number of blocks to reserve
	 */
	ext2fs_r_blocks_count_set(pfs_param, reserved_ratio *
				  ext2fs_blocks_count(pfs_param) / 100.0);

	if (ext2fs_has_feature_sparse_super2(pfs_param)) {
		if (num_backups >= 1)
			pfs_param->s_backup_bgs[0] = 1;
		if (num_backups >= 2)
			pfs_param->s_backup_bgs[1] = ~0;
	}

	free(usage_types);

	return 0;
}

static void fix_cluster_bg_counts(ext2_filsys fs)
{
	blk64_t		block, num_blocks, last_block, next;
	blk64_t		tot_free = 0;
	errcode_t	retval;
	dgrp_t		group = 0;
	int		grp_free = 0;

	num_blocks = ext2fs_blocks_count(fs->super);
	last_block = ext2fs_group_last_block2(fs, group);
	block = fs->super->s_first_data_block;
	while (block < num_blocks) {
		retval = ext2fs_find_first_zero_block_bitmap2(fs->block_map,
						block, last_block, &next);
		if (retval == 0)
			block = next;
		else {
			block = last_block + 1;
			goto next_bg;
		}

		retval = ext2fs_find_first_set_block_bitmap2(fs->block_map,
						block, last_block, &next);
		if (retval)
			next = last_block + 1;
		grp_free += EXT2FS_NUM_B2C(fs, next - block);
		tot_free += next - block;
		block = next;

		if (block > last_block) {
		next_bg:
			ext2fs_bg_free_blocks_count_set(fs, group, grp_free);
			ext2fs_group_desc_csum_set(fs, group);
			grp_free = 0;
			group++;
			last_block = ext2fs_group_last_block2(fs, group);
		}
	}
	ext2fs_free_blocks_count_set(fs->super, tot_free);
}

int ext_mkfs(const char *device_name, const char *fstype, unsigned long features,
		const char *volume_label)
{
	errcode_t	retval = 0;
	ext2_filsys	fs;
	badblocks_list	bb_list = 0;
	unsigned int	journal_blocks = 0;
	io_manager	io_ptr;
	char		opt_string[40];
	int		itable_zeroed = 0;
	unsigned long   flags;
	struct ext2_super_block fs_param;
	uid_t		root_uid = 0;
	gid_t		root_gid = 0;
	blk64_t		journal_location = ~0LL;
	int		lazy_itable_init;
	int		journal_flags = 0;
	int		journal_size = 0;

	memset(&fs_param, 0, sizeof(struct ext2_super_block));

	if (!features) {
		fs_param.s_feature_compat = EXT2_FEATURE_COMPAT_DIR_INDEX |
			EXT2_FEATURE_COMPAT_RESIZE_INODE |
			EXT2_FEATURE_COMPAT_EXT_ATTR;
		fs_param.s_feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE;
		fs_param.s_feature_ro_compat = EXT2_FEATURE_RO_COMPAT_LARGE_FILE |
			EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER;
		if (!strcmp(fstype, "ext4")) {
			fs_param.s_feature_compat |= EXT3_FEATURE_COMPAT_HAS_JOURNAL;
			fs_param.s_feature_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE |
				EXT3_FEATURE_INCOMPAT_EXTENTS |
				EXT4_FEATURE_INCOMPAT_64BIT |
				EXT4_FEATURE_INCOMPAT_FLEX_BG;
			fs_param.s_feature_ro_compat = EXT4_FEATURE_RO_COMPAT_METADATA_CSUM |
				EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE |
				EXT4_FEATURE_RO_COMPAT_DIR_NLINK |
				EXT4_FEATURE_RO_COMPAT_HUGE_FILE |
				EXT2_FEATURE_RO_COMPAT_LARGE_FILE |
				EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER;
			journal_size = -1;
		}
		if (!strcmp(fstype, "ext3")) {
			fs_param.s_feature_compat |= EXT3_FEATURE_COMPAT_HAS_JOURNAL;
			journal_size = -1;
		}
	} else
		fs_param.s_feature_compat = features;

	TRACE("mke2fs parms for %s: compat 0x%x incompat 0x%x ro %x",
		fstype,
		fs_param.s_feature_compat,
		fs_param.s_feature_incompat,
		fs_param.s_feature_ro_compat);

	retval = mkfs_prepare(device_name, &fs_param);

	if (retval)
		return retval;

	io_ptr = unix_io_manager;

	/*
	 * Initialize the superblock....
	 */
	flags = EXT2_FLAG_EXCLUSIVE;
	flags |= EXT2_FLAG_64BITS;

	retval = ext2fs_initialize(device_name, flags, &fs_param,
				   io_ptr, &fs);
	if (retval) {
		ERROR("%s: while setting up superblock", strerror(retval));
		return -EFAULT;
	}

	if (ext2fs_has_feature_csum_seed(fs->super) &&
	    !ext2fs_has_feature_metadata_csum(fs->super)) {
		ERROR("The metadata_csum_seed feature "
		       "requires the metadata_csum feature.");
		return -EINVAL;
	}

	lazy_itable_init = 0;

	if (access("/sys/fs/ext4/features/lazy_itable_init", R_OK) == 0)
		lazy_itable_init = 1;

	/* Calculate journal blocks */
	if (journal_size ||
	    ext2fs_has_feature_journal(&fs_param))
		journal_blocks = figure_journal_size(journal_size, fs);

	sprintf(opt_string, "tdb_data_size=%d", fs->blocksize <= 4096 ?
		32768 : fs->blocksize * 8);
	io_channel_set_options(fs->io, opt_string);
	if (offset) {
		sprintf(opt_string, "offset=%llu", offset);
		io_channel_set_options(fs->io, opt_string);
	}

	if (fs_param.s_flags & EXT2_FLAGS_TEST_FILESYS)
		fs->super->s_flags |= EXT2_FLAGS_TEST_FILESYS;

	if (ext2fs_has_feature_flex_bg(&fs_param) ||
	    ext2fs_has_feature_huge_file(&fs_param) ||
	    ext2fs_has_feature_gdt_csum(&fs_param) ||
	    ext2fs_has_feature_dir_nlink(&fs_param) ||
	    ext2fs_has_feature_metadata_csum(&fs_param) ||
	    ext2fs_has_feature_extra_isize(&fs_param))
		fs->super->s_kbytes_written = 1;

	/*
	 * Wipe out the old on-disk superblock
	 */
	if (zap_sector(fs, 2, 6) < 0)
		return -EFAULT;

	/*
	 * Generate a UUID for the filesystem
	 */
	uuid_generate(fs->super->s_uuid);

	if (ext2fs_has_feature_csum_seed(fs->super))
		fs->super->s_checksum_seed = ext2fs_crc32c_le(~0,
				fs->super->s_uuid, sizeof(fs->super->s_uuid));

	ext2fs_init_csum_seed(fs);

	/*
	 * Initialize the directory index variables
	 */

	if (memcmp(fs_param.s_hash_seed, zero_buf,
		sizeof(fs_param.s_hash_seed)) != 0) {
		memcpy(fs->super->s_hash_seed, fs_param.s_hash_seed,
			sizeof(fs->super->s_hash_seed));
	} else
		uuid_generate((unsigned char *) fs->super->s_hash_seed);

	/*
	 * Do not enable periodic fsck
	 */
	fs->super->s_max_mnt_count = -1;

	/*
	 * Set the volume label...
	 */
	if (volume_label) {
		memset(fs->super->s_volume_name, 0,
		       sizeof(fs->super->s_volume_name));
		strncpy(fs->super->s_volume_name, volume_label,
			sizeof(fs->super->s_volume_name) - 1);
	}

	/* Set current default encryption algorithms for data and
	 * filename encryption */
	if (ext2fs_has_feature_encrypt(fs->super)) {
		fs->super->s_encrypt_algos[0] =
			EXT4_ENCRYPTION_MODE_AES_256_XTS;
		fs->super->s_encrypt_algos[1] =
			EXT4_ENCRYPTION_MODE_AES_256_CTS;
	}

	if (ext2fs_has_feature_metadata_csum(fs->super))
		fs->super->s_checksum_type = EXT2_CRC32C_CHKSUM;

	fs->stride = fs->super->s_raid_stride;
	if (ext2fs_has_feature_flex_bg(fs->super))
		retval = packed_allocate_tables(fs);
	else
		retval = ext2fs_allocate_tables(fs);
	if (retval) {
		ERROR("while trying to allocate filesystem tables");
		return -EFAULT;
	}

	retval = ext2fs_convert_subcluster_bitmap(fs, &fs->block_map);
	if (retval) {
		ERROR("while converting subcluster bitmap");
		return -EFAULT;
	}


	/* rsv must be a power of two (64kB is MD RAID sb alignment) */
	blk64_t rsv = 65536 / fs->blocksize;
	blk64_t blocks = ext2fs_blocks_count(fs->super);
	blk64_t start;
	blk64_t ret_blk;

	if (zap_sector(fs, 0, 2) < 0)
		return -EFAULT;

	/*
	 * Wipe out any old MD RAID (or other) metadata at the end
	 * of the device.  This will also verify that the device is
	 * as large as we think.  Be careful with very small devices.
	 */
	start = (blocks & ~(rsv - 1));
	if (start > rsv)
		start -= rsv;
	if (start > 0)
		retval = ext2fs_zero_blocks2(fs, start, blocks - start,
					    &ret_blk, NULL);

	if (retval) {
		ERROR("while zeroing block %llu at end of filesystem",
			ret_blk);
	}
	if (write_inode_tables(fs, lazy_itable_init, itable_zeroed) < 0 ||
		create_root_dir(fs, root_uid, root_gid) < 0 ||
		create_lost_and_found(fs) < 0)
		return -EFAULT;
	reserve_inodes(fs);
	if (create_bad_block_inode(fs, bb_list) < 0)
		return -EFAULT;
	if (ext2fs_has_feature_resize_inode(fs->super)) {
		retval = ext2fs_create_resize_inode(fs);
		if (retval) {
			ERROR("while reserving blocks for online resize");
			return -EFAULT;
		}
	}

	journal_flags |= EXT2_MKJOURNAL_NO_MNT_CHECK;

	if ((journal_size) ||
	   ext2fs_has_feature_journal(&fs_param)) {

		if (!journal_blocks) {
			ext2fs_clear_feature_journal(fs->super);
			goto no_journal;
		}
		retval = ext2fs_add_journal_inode2(fs, journal_blocks,
						   journal_location,
						   journal_flags);
		if (retval) {
			ERROR("while trying to create journal");
			return -EFAULT;
		}
	}
no_journal:
	if (ext2fs_has_feature_mmp(fs->super)) {
		retval = ext2fs_mmp_init(fs);
		if (retval) {
			ERROR("Error while enabling multiple "
				  "mount protection feature.");
			return -EFAULT;
		}
	}

	if (ext2fs_has_feature_bigalloc(&fs_param))
		fix_cluster_bg_counts(fs);

	retval = ext2fs_close_free(&fs);
	if (retval) {
		ERROR("while writing out and closing file system");
		retval = 1;
	}
	return retval;
}
