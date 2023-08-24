/* SPDX-License-Identifier: MIT */

#include <linux/fs.h>

extern void *pkernfs_mem;
#define PKERNFS_MAGIC_NUMBER 0x6345789
struct pkernfs_sb {
	unsigned long magic_number;
};

// If neither of these are set the inode is not in use.
#define PKERNFS_INODE_FLAG_FILE (1 << 0)
#define PKERNFS_INODE_FLAG_DIR (1 << 1)
struct pkernfs_inode {
	int flags;
	char filename[32];
	int mappings_block;
	int num_mappings;
};

void pkernfs_zero_inode_store(struct super_block *sb);
void pkernfs_zero_allocations(struct super_block *sb);
 struct inode *pkernfs_inode_get(struct super_block *sb, unsigned long ino);

extern const struct file_operations pkernfs_dir_fops;
