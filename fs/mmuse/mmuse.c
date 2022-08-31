/* SPDX-License-Identifier: MIT */

#include "mmuse.h"
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/fs_context.h>

static const struct super_operations mmuse_super_ops = { };

static int mmuse_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *inode;
	struct dentry *dentry;

	sb->s_op = &mmuse_super_ops;

	inode = new_inode(sb);
	if (!inode)
		return -ENOMEM;

	inode->i_ino = get_next_ino();
	inode->i_mode = S_IFDIR;
	inode->i_op = &mmuse_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	/* directory inodes start off with i_nlink == 2 (for "." entry) */
	inc_nlink(inode);

	dentry = d_make_root(inode);
	if (!dentry)
		return -ENOMEM;
	sb->s_root = dentry;

	return 0;
}

static int mmuse_get_tree(struct fs_context *fc)
{
	int ret = get_tree_nodev(fc, mmuse_fill_super);
	return mmuse_create_admin_file(fc->root);
}

static const struct fs_context_operations mmuse_context_ops = {
	.get_tree	= mmuse_get_tree,
};

static int mmuse_init_fs_context(struct fs_context *const fc)
{
	fc->ops = &mmuse_context_ops;
	return 0;
}

static struct file_system_type mmuse_fs_type = {
	.owner                  = THIS_MODULE,
	.name                   = "mmuse",
	.init_fs_context        = mmuse_init_fs_context,
	.kill_sb                = kill_litter_super,
	.fs_flags               = FS_USERNS_MOUNT,
};

static int __init mmuse_init(void)
{
	int ret;
	ret = register_filesystem(&mmuse_fs_type);
	printk("mmuse_init: %i\n", ret);
	return ret;
}

MODULE_ALIAS_FS("mmuse");
module_init(mmuse_init);
