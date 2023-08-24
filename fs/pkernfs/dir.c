/* SPDX-License-Identifier: MIT */

#include "pkernfs.h"

static int pkernfs_dir_iterate(struct file *dir, struct dir_context *ctx)
{
	struct pkernfs_inode *pkernfs_inode;
	printk("pkernfs iterate called\n");

	dir_emit_dots(dir, ctx);

	/* -2 for the dots. */
	for(int inode_idx = (ctx->pos - 2); inode_idx < 32; ++inode_idx) {
		pkernfs_inode = pkernfs_get_persisted_inode(dir->f_inode->i_sb, inode_idx);
		if (pkernfs_inode->flags) {
			dir_emit(ctx, pkernfs_inode->filename, 32,
					inode_idx, DT_UNKNOWN);
			ctx->pos++;
			printk("emitting %s\n", pkernfs_inode->filename);
		}
	}
	return 0;
}

const struct file_operations pkernfs_dir_fops = {
	.owner = THIS_MODULE,
	.iterate_shared = pkernfs_dir_iterate,
};
