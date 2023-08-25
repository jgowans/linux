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

/*
 * Allocs one 2 MiB block, and returns the block index.
 * Index is 2 MiB chunk index.
 */
unsigned long pkernfs_alloc_block(struct super_block *sb)
{
	unsigned long free_bit;

	/* Allocations is 2nd half of first page */
	void *allocations_mem = pkernfs_allocations_bitmap(sb);
	free_bit = bitmap_find_next_zero_area(allocations_mem,
			PMD_SIZE / 2, /* Size */
			0, /* Start */
			1, /* Number of zeroed bits to look for */
			0); /* Alignment mask - none required. */
	bitmap_set(allocations_mem, free_bit, 1);
	return free_bit;
}

void *pkernfs_addr_for_block(struct super_block *sb, int block_idx)
{
	return pkernfs_mem + (block_idx * PMD_SIZE);
}
