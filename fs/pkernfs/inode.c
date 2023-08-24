/* SPDX-License-Identifier: MIT */

#include "pkernfs.h"
#include <linux/fs.h>

const struct inode_operations pkernfs_dir_inode_operations;

static struct pkernfs_inode *pkernfs_get_persisted_inode(struct super_block *sb, int ino) {
	return ((struct pkernfs_inode *) (pkernfs_mem + (2 << 20))) + ino;
}

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

static int pkernfs_get_next_free_inode_no(struct super_block *sb)
{
	/* Inode store is 2nd 2 MiB page */
	struct pkernfs_inode *inode_store_mem = (struct pkernfs_inode *) (pkernfs_mem + (2 << 20));
	for (int inode_idx = 0; (inode_idx < ((2 << 20)) / sizeof(struct pkernfs_inode)); inode_idx++) {
	    if (!inode_store_mem[inode_idx].flags) {
		return inode_idx;
	    }
	}
	return -ENOMEM;
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
	int free_inode;
	struct pkernfs_inode *pkernfs_inode;
	struct inode *vfs_inode;
	printk("pkernfs_create invoked for %s\n", dentry->d_name.name);

	free_inode = pkernfs_get_next_free_inode_no(dir->i_sb);
	if (free_inode < 0)
	    return free_inode;

	vfs_inode = pkernfs_inode_get(dir->i_sb, free_inode);
	pkernfs_inode = pkernfs_get_persisted_inode(dir->i_sb, free_inode);
	strncpy(pkernfs_inode->filename, dentry->d_name.name, 32);

	printk("next free inode: %i\n", free_inode);
	return 0;
}

static struct dentry *pkernfs_lookup(struct inode *dir,
		struct dentry *dentry,
		unsigned int flags)
{
	printk("pkernfs_lookup invoked for %s\n", dentry->d_name.name);
	return NULL;
}

const struct inode_operations pkernfs_dir_inode_operations = {
	.create		= pkernfs_create,
	.lookup		= pkernfs_lookup,
};
