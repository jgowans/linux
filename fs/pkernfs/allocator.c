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

/*
 * Allocs one 2 MiB block, and returns the block index.
 * Index is 2 MiB chunk index.
 */
unsigned long pkernfs_alloc_block(struct super_block *sb)
{
	unsigned long free_bit;
	/* Allocations is 2nd half of firist page */
	void *allocations_mem = pkernfs_mem + (1 << 20);
	free_bit = bitmap_find_next_zero_area(allocations_mem,
			(1 << 20), /* Size */
			0, /* Start */
			1, /* Number of zeroed bits to look for */
			0); /* Alignment mask - none required. */
	bitmap_set(allocations_mem, free_bit, 1);
	return free_bit;
}

void *pkernfs_addr_for_block(struct super_block *sb, int block_idx)
{
    return pkernfs_mem + (block_idx * (2 << 20));
}
