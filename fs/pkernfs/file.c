// SPDX-License-Identifier: GPL-2.0-only

#include "pkernfs.h"

static int truncate(struct inode *inode, loff_t newsize)
{
	unsigned long free_block;
	struct pkernfs_inode *pkernfs_inode;
	unsigned long *mappings;

	pkernfs_inode = pkernfs_get_persisted_inode(inode->i_sb, inode->i_ino);
	mappings = (unsigned long *)pkernfs_addr_for_block(inode->i_sb,
		pkernfs_inode->mappings_block);
	i_size_write(inode, newsize);
	for (int block_idx = 0; block_idx * PMD_SIZE < newsize; ++block_idx) {
		free_block = pkernfs_alloc_block(inode->i_sb);
		if (free_block <= 0)
			/* TODO: roll back allocations. */
			return -ENOMEM;
		*(mappings + block_idx) = free_block;
		++pkernfs_inode->num_mappings;
	}
	return 0;
}

static int inode_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = dentry->d_inode;
	int error;

	error = setattr_prepare(idmap, dentry, iattr);
	if (error)
		return error;

	if (iattr->ia_valid & ATTR_SIZE) {
		error = truncate(inode, iattr->ia_size);
		if (error)
			return error;
	}
	setattr_copy(idmap, inode, iattr);
	mark_inode_dirty(inode);
	return 0;
}

const struct inode_operations pkernfs_file_inode_operations = {
	.setattr = inode_setattr,
	.getattr = simple_getattr,
};

const struct file_operations pkernfs_file_fops = {
	.owner = THIS_MODULE,
	.iterate_shared = NULL,
};
