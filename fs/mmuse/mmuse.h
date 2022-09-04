/* SPDX-License-Identifier: MIT */

#include <linux/dcache.h>

extern const struct inode_operations mmuse_dir_inode_operations;

/* Creates an "admin" file in the supplied root dir. */
int mmuse_create_admin_file(struct dentry *root);

/* Created a file which can have memory mappings assigned to it */
int mmuse_create_memory_file(struct dentry *root);

/* --- These should be in uapi --- */
#define MMUSE_ADMIN_IOCTL_SET_BACKING_FILE _IO('m', 0x100)
