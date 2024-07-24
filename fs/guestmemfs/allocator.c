// SPDX-License-Identifier: GPL-2.0-only

#include "guestmemfs.h"

/**
 * For allocating blocks from the guestmemfs filesystem.
 */

static void *guestmemfs_allocations_bitmap(struct super_block *sb)
{
	return GUESTMEMFS_PSB(sb)->allocator_bitmap;
}

void guestmemfs_zero_allocations(struct super_block *sb)
{
	memset(guestmemfs_allocations_bitmap(sb), 0, (1 << 20));
}

/*
 * Allocs one 2 MiB block, and returns the block index.
 * Index is 2 MiB chunk index.
 * Negative error code if unable to alloc.
 */
long guestmemfs_alloc_block(struct super_block *sb)
{
	unsigned long free_bit;
	void *allocations_mem = guestmemfs_allocations_bitmap(sb);

	free_bit = bitmap_find_next_zero_area(allocations_mem,
			(1 << 20), /* Size */
			0, /* Start */
			1, /* Number of zeroed bits to look for */
			0); /* Alignment mask - none required. */

	if (free_bit >= PMD_SIZE / 2)
		return -ENOMEM;

	bitmap_set(allocations_mem, free_bit, 1);
	return free_bit;
}
