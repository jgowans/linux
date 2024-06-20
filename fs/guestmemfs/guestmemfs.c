// SPDX-License-Identifier: GPL-2.0-only

#include "guestmemfs.h"
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/kexec.h>
#include <linux/module.h>
#include <linux/fs_context.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/statfs.h>

phys_addr_t guestmemfs_base, guestmemfs_size;
struct super_block *guestmemfs_sb;

static int statfs(struct dentry *root, struct kstatfs *buf)
{
	simple_statfs(root, buf);
	buf->f_bsize = PMD_SIZE;
	buf->f_blocks = guestmemfs_size / PMD_SIZE;
	buf->f_bfree = buf->f_bavail = buf->f_blocks;
	buf->f_files = PMD_SIZE / sizeof(struct guestmemfs_inode);
	buf->f_ffree = buf->f_files -
		GUESTMEMFS_PSB(root->d_sb)->allocated_inodes;
	return 0;
}

static const struct super_operations guestmemfs_super_ops = {
	.statfs = statfs,
};

static int guestmemfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *inode;
	struct dentry *dentry;

	/*
	 * Keep a reference to the persistent super block in the
	 * ephemeral super block.
	 */
	sb->s_fs_info = guestmemfs_restore_from_kho();

	if (GUESTMEMFS_PSB(sb)) {
		pr_info("Restored super block from KHO\n");
	} else {
		struct guestmemfs_sb *psb;

		pr_info("Did not restore from KHO - allocating free\n");
		psb = kzalloc(sizeof(*psb), GFP_KERNEL);
		psb->inodes = kzalloc(2 << 20, GFP_KERNEL);
		if (!psb->inodes)
			return -ENOMEM;
		psb->allocator_bitmap = kzalloc(1 << 20, GFP_KERNEL);
		if (!psb->allocator_bitmap)
			return -ENOMEM;
		sb->s_fs_info = psb;
		spin_lock_init(&psb->allocation_lock);
		guestmemfs_initialise_inode_store(sb);
		guestmemfs_zero_allocations(sb);
		guestmemfs_get_persisted_inode(sb, 1)->flags = GUESTMEMFS_INODE_FLAG_DIR;
		strscpy(guestmemfs_get_persisted_inode(sb, 1)->filename, ".",
				GUESTMEMFS_FILENAME_LEN);
		GUESTMEMFS_PSB(sb)->next_free_ino = 2;
	}
	/*
	 * Keep a reference to this sb; the serialise callback needs it
	 * and has no oher way to get it.
	 */
	guestmemfs_sb = sb;

	sb->s_op = &guestmemfs_super_ops;

	inode = guestmemfs_inode_get(sb, 1);
	if (!inode)
		return -ENOMEM;

	inode->i_mode = S_IFDIR;
	inode->i_fop = &guestmemfs_dir_fops;
	simple_inode_init_ts(inode);
	/* directory inodes start off with i_nlink == 2 (for "." entry) */
	inc_nlink(inode);
	inode_init_owner(&nop_mnt_idmap, inode, NULL, inode->i_mode);

	dentry = d_make_root(inode);
	if (!dentry)
		return -ENOMEM;
	sb->s_root = dentry;

	return 0;
}

static int guestmemfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, guestmemfs_fill_super);
}

static const struct fs_context_operations guestmemfs_context_ops = {
	.get_tree	= guestmemfs_get_tree,
};

static int guestmemfs_init_fs_context(struct fs_context *const fc)
{
	fc->ops = &guestmemfs_context_ops;
	return 0;
}

static struct file_system_type guestmemfs_fs_type = {
	.owner                  = THIS_MODULE,
	.name                   = "guestmemfs",
	.init_fs_context        = guestmemfs_init_fs_context,
	.kill_sb                = kill_litter_super,
	.fs_flags               = FS_USERNS_MOUNT,
};


static struct notifier_block trace_kho_nb = {
	.notifier_call = guestmemfs_serialise_to_kho,
};

static int __init guestmemfs_init(void)
{
	int ret;

	ret = register_filesystem(&guestmemfs_fs_type);
	if (IS_ENABLED(CONFIG_FTRACE_KHO))
		register_kho_notifier(&trace_kho_nb);
	return ret;
}

/**
 * Format: guestmemfs=<size>:<base>
 * Just like: memmap=nn[KMG]!ss[KMG]
 */
static int __init parse_guestmemfs_extents(char *p)
{
	guestmemfs_size = memparse(p, &p);
	return 0;
}

early_param("guestmemfs", parse_guestmemfs_extents);

void __init guestmemfs_reserve_mem(void)
{
	if (guestmemfs_size) {
		guestmemfs_base = memblock_phys_alloc(guestmemfs_size, 4 << 10);

		if (guestmemfs_base) {
			memblock_reserved_mark_noinit(guestmemfs_base, guestmemfs_size);
			memblock_mark_nomap(guestmemfs_base, guestmemfs_size);
			pr_debug("guestmemfs reserved base=%llu from memblocks\n", guestmemfs_base);
		} else {
			pr_warn("Failed to alloc %llu bytes for guestmemfs\n", guestmemfs_size);
		}
	}

}

MODULE_ALIAS_FS("guestmemfs");
module_init(guestmemfs_init);
