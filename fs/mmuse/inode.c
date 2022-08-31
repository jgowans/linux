/* SPDX-License-Identifier: MIT */

#include "mmuse.h"
#include <linux/fs.h>
#include <linux/ramfs.h>

const struct inode_operations file_inode_operations = {
	.setattr	= simple_setattr,
	.getattr	= simple_getattr,
};

int mmuse_create_admin_file(struct dentry *root)
{
	struct dentry *dentry;
	struct inode *inode;

	dentry = d_alloc_name(root, "admin");
	dget(dentry);	/* Extra count - pin the dentry in core */
	inc_nlink(d_inode(root));

	if (!dentry)
		return -ENOMEM;
	dget(dentry);	/* Extra count - pin the dentry in core */

	inode = new_inode(root->d_sb);
	if (!inode)
		return -ENOMEM;
	inode_init_owner(&init_user_ns, inode, root->d_inode, S_IFREG | 0644);
	inode->i_ino = get_next_ino();
	inode->i_blocks = 0;
	//inode->i_fop = &ramfs_file_operations;
	inode->i_op = &file_inode_operations;
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	inc_nlink(inode);
	d_instantiate(dentry, inode);
	d_add(dentry, inode);
	root->d_inode->i_mtime = root->d_inode->i_ctime = current_time(root->d_inode);
	return 0;
}
