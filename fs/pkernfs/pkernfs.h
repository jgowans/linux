/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/fs.h>

#define PKERNFS_MAGIC_NUMBER 0x706b65726e6673
#define PKERNFS_FILENAME_LEN 255

extern void *pkernfs_mem;

struct pkernfs_sb {
	unsigned long magic_number;
	/* Inode number */
	unsigned long next_free_ino;
};

// If neither of these are set the inode is not in use.
#define PKERNFS_INODE_FLAG_FILE (1 << 0)
#define PKERNFS_INODE_FLAG_DIR (1 << 1)
struct pkernfs_inode {
	int flags;
	/*
	 * Points to next inode in the same directory, or
	 * 0 if last file in directory.
	 */
	unsigned long sibling_ino;
	/*
	 * If this inode is a directory, this points to the
	 * first inode *in* that directory.
	 */
	unsigned long child_ino;
	char filename[PKERNFS_FILENAME_LEN];
	int mappings_block;
	int num_mappings;
};

void pkernfs_initialise_inode_store(struct super_block *sb);
void pkernfs_zero_allocations(struct super_block *sb);
struct inode *pkernfs_inode_get(struct super_block *sb, unsigned long ino);
struct pkernfs_inode *pkernfs_get_persisted_inode(struct super_block *sb, int ino);

extern const struct file_operations pkernfs_dir_fops;
