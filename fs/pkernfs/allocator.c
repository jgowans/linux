/* SPDX-License-Identifier: MIT */

#include "pkernfs.h"

void pkernfs_zero_allocations(struct super_block *sb)
{
	/* Allocations is 2nd half of firist page */
	void *inode_store_mem = pkernfs_mem + (1 << 20);
	memset(inode_store_mem, 0, (1 << 20));
	/* First page is persisted super block and allocator bitmap */
	set_bit(0, inode_store_mem);
	/* Second page is inode store */
	set_bit(1, inode_store_mem);
}
