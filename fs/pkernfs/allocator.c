// SPDX-License-Identifier: GPL-2.0-only

#include "pkernfs.h"

/**
 * For allocating blocks from the pkernfs filesystem.
 * The first two blocks are special:
 * - the first block is persitent filesystme metadata and
 *   a bitmap of allocated blocks
 * - the second block is an array of persisted inodes; the
 *   inode store.
 */

static void *pkernfs_allocations_bitmap(struct super_block *sb)
{
	/* Allocations is 2nd half of first block */
	return pkernfs_mem + (1 << 20);
}

void pkernfs_zero_allocations(struct super_block *sb)
{
	memset(pkernfs_allocations_bitmap(sb), 0, (1 << 20));
	/* First page is persisted super block and allocator bitmap */
	set_bit(0, pkernfs_allocations_bitmap(sb));
	/* Second page is inode store */
	set_bit(1, pkernfs_allocations_bitmap(sb));
}
