/* SPDX-License-Identifier: GPL-2.0-only */

#define pr_fmt(fmt) "guestmemfs: " KBUILD_MODNAME ": " fmt

#include <linux/guestmemfs.h>
#include <linux/fs.h>

#define GUESTMEMFS_FILENAME_LEN 255
#define GUESTMEMFS_PSB(sb) ((struct guestmemfs_sb *)sb->s_fs_info)

struct guestmemfs_sb {
	/* Inode number */
	unsigned long next_free_ino;
	unsigned long allocated_inodes;
	struct guestmemfs_inode *inodes;
	void *allocator_bitmap;
	spinlock_t allocation_lock;
};

// If neither of these are set the inode is not in use.
#define GUESTMEMFS_INODE_FLAG_FILE (1 << 0)
#define GUESTMEMFS_INODE_FLAG_DIR (1 << 1)
struct guestmemfs_inode {
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
	char filename[GUESTMEMFS_FILENAME_LEN];
	void *mappings;
	int num_mappings;
};

void guestmemfs_initialise_inode_store(struct super_block *sb);
void guestmemfs_zero_allocations(struct super_block *sb);
long guestmemfs_alloc_block(struct super_block *sb);
struct inode *guestmemfs_inode_get(struct super_block *sb, unsigned long ino);
struct guestmemfs_inode *guestmemfs_get_persisted_inode(struct super_block *sb, int ino);

extern const struct file_operations guestmemfs_dir_fops;
extern const struct file_operations guestmemfs_file_fops;
extern const struct inode_operations guestmemfs_file_inode_operations;
