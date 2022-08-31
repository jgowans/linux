/* SPDX-License-Identifier: MIT */

#include <linux/dcache.h>

extern const struct inode_operations mmuse_dir_inode_operations;

/* Creates an "admin" file in the supplied root dir. */
int mmuse_create_admin_file(struct dentry *root);
