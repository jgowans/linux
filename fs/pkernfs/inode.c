/* SPDX-License-Identifier: MIT */

#include "pkernfs.h"
#include <linux/fs.h>

const struct inode_operations pkernfs_dir_inode_operations;

struct inode *pkernfs_inode_get(struct super_block *sb, unsigned long ino) {
	struct inode *inode;
	inode = iget_locked(sb, ino);

	/* If this inode is cached it is already populated; just return */
	if (!(inode->i_state & I_NEW)) {
		printk("Aleady populated inode %lu\n", ino);
		return inode;
	}
	printk("New inode %lu\n", ino);
	inode->i_op = &pkernfs_dir_inode_operations;
	inode->i_sb = sb;
	unlock_new_inode(inode);
	return inode;
}



void pkernfs_zero_inode_store(struct super_block *sb)
{
	/* Inode store is 2nd 2 MiB page */
	void *inode_store_mem = pkernfs_mem + (2 << 20);
	memset(inode_store_mem, 0, (2 << 20));
}

static int pkernfs_create(struct mnt_idmap *id,
		struct inode *dir,
		struct dentry *dentry,
		umode_t mode,
		bool excl) {
	printk("pkernfs_create invoked\n");
	return 0;
}

static struct dentry *pkernfs_lookup(struct inode *dir,
		struct dentry *dentry,
		unsigned int flags)
{
	printk("pkernfs_lookup invoked\n");
	return NULL;
}

const struct inode_operations pkernfs_dir_inode_operations = {
	.create		= pkernfs_create,
	.lookup		= pkernfs_lookup,
};
