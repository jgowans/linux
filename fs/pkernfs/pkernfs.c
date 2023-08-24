/* SPDX-License-Identifier: MIT */

#include "pkernfs.h"
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/fs_context.h>
#include <linux/io.h>

static phys_addr_t base, size;
void *pkernfs_mem;
static const struct super_operations pkernfs_super_ops = { };

static int pkernfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *inode;
	struct dentry *dentry;

	struct pkernfs_sb *psb = (struct pkernfs_sb *) pkernfs_mem;
	if (psb->magic_number == PKERNFS_MAGIC_NUMBER) {
	    printk("pkernfs Restoring from super block\n");
	} else {
	    printk("pkernfs Clean super block; initialising\n");
	    pkernfs_zero_inode_store(sb);
	    pkernfs_zero_allocations(sb);
	    psb->magic_number = PKERNFS_MAGIC_NUMBER;
	}

	sb->s_op = &pkernfs_super_ops;

	inode = pkernfs_inode_get(sb, 0);
	if (!inode)
		return -ENOMEM;

	inode->i_mode = S_IFDIR;
	inode->i_fop = &pkernfs_dir_fops;
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	/* directory inodes start off with i_nlink == 2 (for "." entry) */
	inc_nlink(inode);
	inode_init_owner(&nop_mnt_idmap, inode, NULL, inode->i_mode);

	dentry = d_make_root(inode);
	if (!dentry)
		return -ENOMEM;
	sb->s_root = dentry;

	printk("pkernfs_fill_super complete\n");
	return 0;
}

static int pkernfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, pkernfs_fill_super);
}

static const struct fs_context_operations pkernfs_context_ops = {
	.get_tree	= pkernfs_get_tree,
};

static int pkernfs_init_fs_context(struct fs_context *const fc)
{
	fc->ops = &pkernfs_context_ops;
	return 0;
}

static struct file_system_type pkernfs_fs_type = {
	.owner                  = THIS_MODULE,
	.name                   = "pkernfs",
	.init_fs_context        = pkernfs_init_fs_context,
	.kill_sb                = kill_litter_super,
	.fs_flags               = FS_USERNS_MOUNT,
};

static int __init pkernfs_init(void)
{
	int ret;
	ret = register_filesystem(&pkernfs_fs_type);
	/* requires nopat */
	pkernfs_mem = ioremap(base,size);
	printk("pkernfs_init: %i ; vaddr: %px\n", ret, pkernfs_mem);
	return ret;
}

/**
 * Format: pkernfs=<size>:<base>
 * Just like: memmap=nn[KMG]!ss[KMG]
 */
static int __init parse_pkernfs_extents(char *p)
{
        size = memparse(p, &p);
	printk("pkernfs size: %llu\n", size);
	p++; /* Skip over ! char */
	base = memparse(p, &p);
	printk("pkernfs base: %llu\n", base);
	return 0;
}

early_param("pkernfs", parse_pkernfs_extents);

MODULE_ALIAS_FS("pkernfs");
module_init(pkernfs_init);
