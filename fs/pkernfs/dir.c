// SPDX-License-Identifier: GPL-2.0-only

#include "pkernfs.h"

static int pkernfs_dir_iterate(struct file *dir, struct dir_context *ctx)
{
	struct pkernfs_inode *pkernfs_inode;
	struct super_block *sb = dir->f_inode->i_sb;

	/* Indication from previous invoke that there's no more to iterate. */
	if (ctx->pos == -1)
		return 0;

	if (!dir_emit_dots(dir, ctx))
		return 0;

	/*
	 * Just emitted this dir; go to dir contents. Use pos to smuggle
	 * the next inode number to emit across iterations.
	 * -1 indicates no valid inode. Can't use 0 because first loop has pos=0
	 */
	if (ctx->pos == 2) {
		ctx->pos = pkernfs_get_persisted_inode(sb, dir->f_inode->i_ino)->child_ino;
		/* Empty dir case. */
		if (ctx->pos == 0)
			ctx->pos = -1;
	}

	while (ctx->pos > 1) {
		pkernfs_inode = pkernfs_get_persisted_inode(sb, ctx->pos);
		dir_emit(ctx, pkernfs_inode->filename, PKERNFS_FILENAME_LEN,
				ctx->pos, DT_UNKNOWN);
		ctx->pos = pkernfs_inode->sibling_ino;
		if (!ctx->pos)
			ctx->pos = -1;
	}
	return 0;
}

const struct file_operations pkernfs_dir_fops = {
	.owner = THIS_MODULE,
	.iterate_shared = pkernfs_dir_iterate,
};
