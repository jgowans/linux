/* SPDX-License-Identifier: MIT */

#include "mmuse.h"
#include <linux/fs.h>
#include <linux/ramfs.h>

struct admin_inode_data {
	bool set_up;
};

const struct inode_operations file_inode_operations = {
	.setattr	= simple_setattr,
	.getattr	= simple_getattr,
};

struct inode *mmuse_get_inode(struct super_block *sb,
				const struct inode *dir, umode_t mode, dev_t dev)
{
	struct inode * inode = new_inode(sb);

	if (inode) {
		inode->i_ino = get_next_ino();
		inode_init_owner(&init_user_ns, inode, dir, mode);
		inode->i_mapping->a_ops = &ram_aops;
		//mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
		//mapping_set_unevictable(inode->i_mapping);
		inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
		switch (mode & S_IFMT) {
		default:
			BUG();
			break;
		case S_IFREG:
			inode->i_op = &file_inode_operations;
			break;
		case S_IFDIR:
		case S_IFLNK:
			BUG();
		}
	}
	return inode;
}

static int mmuse_mknod(struct user_namespace *mnt_userns, struct inode *dir,
	    struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode * inode = mmuse_get_inode(dir->i_sb, dir, mode, dev);
	int error = -ENOSPC;

	if (inode) {
		d_instantiate(dentry, inode);
		dget(dentry);	/* Extra count - pin the dentry in core */
		error = 0;
		dir->i_mtime = dir->i_ctime = current_time(dir);
	}
	return error;
}
static int mmuse_create(struct user_namespace *mnt_userns, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
{
	return mmuse_mknod(&init_user_ns, dir, dentry, mode | S_IFREG, 0);
}

const struct inode_operations mmuse_dir_inode_operations = {
	.create		= mmuse_create,
	.lookup		= simple_lookup,
};

static int admin_open(struct inode *node, struct file *filp)
{
	printk("admin file opened\n");
	return 0;
}

static long admin_ioctl_set_backing_file(struct file *filp, unsigned long arg)
{
	printk("would set backing file\n");
	return 0;
}
static long admin_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	printk("ioctl %u invoked on admin file\n", cmd);
	switch (cmd) {
	case MMUSE_ADMIN_IOCTL_SET_BACKING_FILE:
		return admin_ioctl_set_backing_file(filp, arg);
	default:
		printk("invalid ioctl\n");
		return -EINVAL;
	}
}

const struct file_operations admin_fops = {
	.open = admin_open,
	.unlocked_ioctl = admin_ioctl,
};

int mmuse_create_admin_file(struct dentry *root)
{
	struct dentry *dentry;
	struct inode *inode;
	struct admin_inode_data *private;

	private = kzalloc(sizeof(*private), GFP_KERNEL);

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
	inode->i_fop = &admin_fops;
	inode->i_op = &file_inode_operations;
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	inode->i_private = private;
	inc_nlink(inode);
	d_instantiate(dentry, inode);
	d_add(dentry, inode);
	root->d_inode->i_mtime = root->d_inode->i_ctime = current_time(root->d_inode);
	return 0;
}
