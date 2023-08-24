// SPDX-License-Identifier: GPL-2.0-only

#include "pkernfs.h"
#include <linux/fs.h>

const struct inode_operations pkernfs_dir_inode_operations;

struct pkernfs_inode *pkernfs_get_persisted_inode(struct super_block *sb, int ino)
{
	/*
	 * Inode index starts at 1, so -1 to get memory index.
	 */
	return ((struct pkernfs_inode *) (pkernfs_mem + PMD_SIZE)) + ino - 1;
}

struct inode *pkernfs_inode_get(struct super_block *sb, unsigned long ino)
{
	struct inode *inode = iget_locked(sb, ino);

	/* If this inode is cached it is already populated; just return */
	if (!(inode->i_state & I_NEW))
		return inode;
	inode->i_op = &pkernfs_dir_inode_operations;
	inode->i_sb = sb;
	inode->i_mode = S_IFREG;
	unlock_new_inode(inode);
	return inode;
}

static unsigned long pkernfs_allocate_inode(struct super_block *sb)
{

	unsigned long next_free_ino;
	struct pkernfs_sb *psb = (struct pkernfs_sb *) pkernfs_mem;

	next_free_ino = psb->next_free_ino;
	if (!next_free_ino)
		return -ENOMEM;
	psb->next_free_ino =
		pkernfs_get_persisted_inode(sb, next_free_ino)->sibling_ino;
	return next_free_ino;
}

/*
 * Zeroes the inode and makes it the head of the free list.
 */
static void pkernfs_free_inode(struct super_block *sb, unsigned long ino)
{
	struct pkernfs_sb *psb = (struct pkernfs_sb *) pkernfs_mem;
	struct pkernfs_inode *inode = pkernfs_get_persisted_inode(sb, ino);

	memset(inode, 0, sizeof(struct pkernfs_inode));
	inode->sibling_ino = psb->next_free_ino;
	psb->next_free_ino = ino;
}

void pkernfs_initialise_inode_store(struct super_block *sb)
{
	/* Inode store is a PMD sized (ie: 2 MiB) page */
	memset(pkernfs_get_persisted_inode(sb, 1), 0, PMD_SIZE);
	/* Point each inode for the next one; linked-list initialisation. */
	for (unsigned long ino = 2; ino * sizeof(struct pkernfs_inode) < PMD_SIZE; ino++)
		pkernfs_get_persisted_inode(sb, ino - 1)->sibling_ino = ino;
}

static int pkernfs_create(struct mnt_idmap *id, struct inode *dir,
			  struct dentry *dentry, umode_t mode, bool excl)
{
	unsigned long free_inode;
	struct pkernfs_inode *pkernfs_inode;
	struct inode *vfs_inode;

	free_inode = pkernfs_allocate_inode(dir->i_sb);
	if (free_inode <= 0)
		return -ENOMEM;

	pkernfs_inode = pkernfs_get_persisted_inode(dir->i_sb, free_inode);
	pkernfs_inode->sibling_ino = pkernfs_get_persisted_inode(dir->i_sb, dir->i_ino)->child_ino;
	pkernfs_get_persisted_inode(dir->i_sb, dir->i_ino)->child_ino = free_inode;
	strscpy(pkernfs_inode->filename, dentry->d_name.name, PKERNFS_FILENAME_LEN);
	pkernfs_inode->flags = PKERNFS_INODE_FLAG_FILE;

	vfs_inode = pkernfs_inode_get(dir->i_sb, free_inode);
	d_instantiate(dentry, vfs_inode);
	return 0;
}

static struct dentry *pkernfs_lookup(struct inode *dir,
		struct dentry *dentry,
		unsigned int flags)
{
	struct pkernfs_inode *pkernfs_inode;
	unsigned long ino;

	pkernfs_inode = pkernfs_get_persisted_inode(dir->i_sb, dir->i_ino);
	ino = pkernfs_inode->child_ino;
	while (ino) {
		pkernfs_inode = pkernfs_get_persisted_inode(dir->i_sb, ino);
		if (!strncmp(pkernfs_inode->filename, dentry->d_name.name, PKERNFS_FILENAME_LEN)) {
			d_add(dentry, pkernfs_inode_get(dir->i_sb, ino));
			break;
		}
		ino = pkernfs_inode->sibling_ino;
	}
	return NULL;
}

static int pkernfs_unlink(struct inode *dir, struct dentry *dentry)
{
	unsigned long ino;
	struct pkernfs_inode *inode;

	ino = pkernfs_get_persisted_inode(dir->i_sb, dir->i_ino)->child_ino;

	/* Special case for first file in dir */
	if (ino == dentry->d_inode->i_ino) {
		pkernfs_get_persisted_inode(dir->i_sb, dir->i_ino)->child_ino =
			pkernfs_get_persisted_inode(dir->i_sb, dentry->d_inode->i_ino)->sibling_ino;
		pkernfs_free_inode(dir->i_sb, ino);
		return 0;
	}

	/*
	 * Although we know exactly the inode to free, because we maintain only
	 * a singly linked list we need to scan for it to find the previous
	 * element so it's "next" pointer can be updated.
	 */
	while (ino) {
		inode = pkernfs_get_persisted_inode(dir->i_sb, ino);
		/* We've found the one pointing to the one we want to delete */
		if (inode->sibling_ino == dentry->d_inode->i_ino) {
			inode->sibling_ino =
				pkernfs_get_persisted_inode(dir->i_sb,
						dentry->d_inode->i_ino)->sibling_ino;
			pkernfs_free_inode(dir->i_sb, dentry->d_inode->i_ino);
			break;
		}
		ino = pkernfs_get_persisted_inode(dir->i_sb, ino)->sibling_ino;
	}

	return 0;
}

const struct inode_operations pkernfs_dir_inode_operations = {
	.create		= pkernfs_create,
	.lookup		= pkernfs_lookup,
	.unlink		= pkernfs_unlink,
};
