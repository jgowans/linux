/* SPDX-License-Identifier: MIT */

#include "pkernfs.h"

static int pkernfs_dir_iterate(struct file *dir, struct dir_context *ctx)
{
	printk("pkernfs iterate called\n");
	return 0;
}

const struct file_operations pkernfs_dir_fops = {
	.owner = THIS_MODULE,
	.iterate_shared = pkernfs_dir_iterate,
};
