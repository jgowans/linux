// SPDX-License-Identifier: GPL-2.0-only

#include "guestmemfs.h"

static int guestmemfs_dir_iterate(struct file *dir, struct dir_context *ctx)
{
	struct guestmemfs_inode *guestmemfs_inode;
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
		ctx->pos = guestmemfs_get_persisted_inode(sb, dir->f_inode->i_ino)->child_ino;
		/* Empty dir case. */
		if (ctx->pos == 0)
			ctx->pos = -1;
	}

	while (ctx->pos > 1) {
		guestmemfs_inode = guestmemfs_get_persisted_inode(sb, ctx->pos);
		dir_emit(ctx, guestmemfs_inode->filename, GUESTMEMFS_FILENAME_LEN,
				ctx->pos, DT_UNKNOWN);
		ctx->pos = guestmemfs_inode->sibling_ino;
		if (!ctx->pos)
			ctx->pos = -1;
	}
	return 0;
}

const struct file_operations guestmemfs_dir_fops = {
	.owner = THIS_MODULE,
	.iterate_shared = guestmemfs_dir_iterate,
};
