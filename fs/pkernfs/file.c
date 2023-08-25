/* SPDX-License-Identifier: MIT */

#include "pkernfs.h"

static int truncate(struct inode *inode, loff_t newsize)
{
	printk("pkernfs_truncate invoked with size %llu\n", newsize);
	i_size_write(inode, newsize);
	return 0;
}

static int inode_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = dentry->d_inode;
	int error;

	printk("inode_setattr\n");

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
