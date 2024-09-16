// SPDX-License-Identifier: GPL-2.0-only

#include "guestmemfs.h"
#include <linux/fs.h>

const struct inode_operations guestmemfs_dir_inode_operations;

struct guestmemfs_inode *guestmemfs_get_persisted_inode(struct super_block *sb, int ino)
{
	/*
	 * Inode index starts at 1, so -1 to get memory index.
	 */
	return GUESTMEMFS_PSB(sb)->inodes + ino - 1;
}

struct inode *guestmemfs_inode_get(struct super_block *sb, unsigned long ino)
{
	struct guestmemfs_inode *guestmemfs_inode;
	struct inode *inode = iget_locked(sb, ino);

	/* If this inode is cached it is already populated; just return */
	if (!(inode->i_state & I_NEW))
		return inode;
	guestmemfs_inode = guestmemfs_get_persisted_inode(sb, ino);
	inode->i_sb = sb;

	if (guestmemfs_inode->flags & GUESTMEMFS_INODE_FLAG_DIR) {
		inode->i_op = &guestmemfs_dir_inode_operations;
		inode->i_mode = S_IFDIR;
	} else {
		inode->i_op = &guestmemfs_file_inode_operations;
		inode->i_mode = S_IFREG;
		inode->i_fop = &guestmemfs_file_fops;
		inode->i_size = guestmemfs_inode->num_mappings * PMD_SIZE;
	}

	set_nlink(inode, 1);

	/* Switch based on file type */
	unlock_new_inode(inode);
	return inode;
}

static unsigned long guestmemfs_allocate_inode(struct super_block *sb)
{

	unsigned long next_free_ino = -ENOMEM;
	struct guestmemfs_sb *psb = GUESTMEMFS_PSB(sb);

	spin_lock(&psb->allocation_lock);

	if (psb->serialised) {
	    spin_unlock(&psb->allocation_lock);
	    return -EBUSY;
	}

	next_free_ino = psb->next_free_ino;
	psb->allocated_inodes += 1;
	if (!next_free_ino)
		goto out;
	psb->next_free_ino =
		guestmemfs_get_persisted_inode(sb, next_free_ino)->sibling_ino;
out:
	spin_unlock(&psb->allocation_lock);
	return next_free_ino;
}

/*
 * Zeroes the inode and makes it the head of the free list.
 */
static void guestmemfs_free_inode(struct super_block *sb, unsigned long ino)
{
	struct guestmemfs_sb *psb = GUESTMEMFS_PSB(sb);
	struct guestmemfs_inode *inode = guestmemfs_get_persisted_inode(sb, ino);

	spin_lock(&psb->allocation_lock);
	memset(inode, 0, sizeof(struct guestmemfs_inode));
	inode->sibling_ino = psb->next_free_ino;
	psb->next_free_ino = ino;
	psb->allocated_inodes -= 1;
	spin_unlock(&psb->allocation_lock);
}

/*
 * Sets all inodes as free and points each free inode to the next one.
 */
void guestmemfs_initialise_inode_store(struct super_block *sb)
{
	/* Inode store is a PMD sized (ie: 2 MiB) page */
	memset(guestmemfs_get_persisted_inode(sb, 1), 0, PMD_SIZE);
	/* Point each inode for the next one; linked-list initialisation. */
	for (unsigned long ino = 2; ino * sizeof(struct guestmemfs_inode) < PMD_SIZE; ino++)
		guestmemfs_get_persisted_inode(sb, ino - 1)->sibling_ino = ino;
}

static int guestmemfs_create(struct mnt_idmap *id, struct inode *dir,
			  struct dentry *dentry, umode_t mode, bool excl)
{
	unsigned long free_inode;
	struct guestmemfs_inode *guestmemfs_inode;
	struct inode *vfs_inode;

	free_inode = guestmemfs_allocate_inode(dir->i_sb);
	if (free_inode <= 0)
		return -ENOMEM;

	guestmemfs_inode = guestmemfs_get_persisted_inode(dir->i_sb, free_inode);
	guestmemfs_inode->sibling_ino =
		guestmemfs_get_persisted_inode(dir->i_sb, dir->i_ino)->child_ino;
	guestmemfs_get_persisted_inode(dir->i_sb, dir->i_ino)->child_ino = free_inode;
	strscpy(guestmemfs_inode->filename, dentry->d_name.name, GUESTMEMFS_FILENAME_LEN);
	guestmemfs_inode->flags = GUESTMEMFS_INODE_FLAG_FILE;
	/* TODO: make dynamic */
	guestmemfs_inode->mappings = kzalloc(PAGE_SIZE, GFP_KERNEL);

	vfs_inode = guestmemfs_inode_get(dir->i_sb, free_inode);
	d_instantiate(dentry, vfs_inode);
	return 0;
}

static struct dentry *guestmemfs_lookup(struct inode *dir,
		struct dentry *dentry,
		unsigned int flags)
{
	struct guestmemfs_inode *guestmemfs_inode;
	struct inode *vfs_inode;
	unsigned long ino;

	guestmemfs_inode = guestmemfs_get_persisted_inode(dir->i_sb, dir->i_ino);
	ino = guestmemfs_inode->child_ino;
	while (ino) {
		guestmemfs_inode = guestmemfs_get_persisted_inode(dir->i_sb, ino);
		if (!strncmp(guestmemfs_inode->filename,
			     dentry->d_name.name,
			     GUESTMEMFS_FILENAME_LEN)) {
			vfs_inode = guestmemfs_inode_get(dir->i_sb, ino);
			mark_inode_dirty(dir);
			inode_update_timestamps(vfs_inode, S_ATIME);
			d_add(dentry, vfs_inode);
			break;
		}
		ino = guestmemfs_inode->sibling_ino;
	}
	return NULL;
}

static int guestmemfs_unlink(struct inode *dir, struct dentry *dentry)
{
	unsigned long ino;
	struct guestmemfs_inode *inode;

	ino = guestmemfs_get_persisted_inode(dir->i_sb, dir->i_ino)->child_ino;

	inode = guestmemfs_get_persisted_inode(dir->i_sb, dentry->d_inode->i_ino);
	if (atomic_read(&inode->long_term_pins))
		return -EBUSY;

	/* Special case for first file in dir */
	if (ino == dentry->d_inode->i_ino) {
		guestmemfs_get_persisted_inode(dir->i_sb, dir->i_ino)->child_ino =
			guestmemfs_get_persisted_inode(dir->i_sb,
					dentry->d_inode->i_ino)->sibling_ino;
		guestmemfs_free_inode(dir->i_sb, ino);
		return 0;
	}

	/*
	 * Although we know exactly the inode to free, because we maintain only
	 * a singly linked list we need to scan for it to find the previous
	 * element so it's "next" pointer can be updated.
	 */
	while (ino) {
		inode = guestmemfs_get_persisted_inode(dir->i_sb, ino);
		/* We've found the one pointing to the one we want to delete */
		if (inode->sibling_ino == dentry->d_inode->i_ino) {
			inode->sibling_ino =
				guestmemfs_get_persisted_inode(dir->i_sb,
						dentry->d_inode->i_ino)->sibling_ino;
			guestmemfs_free_inode(dir->i_sb, dentry->d_inode->i_ino);
			break;
		}
		ino = guestmemfs_get_persisted_inode(dir->i_sb, ino)->sibling_ino;
	}

	return 0;
}

const struct inode_operations guestmemfs_dir_inode_operations = {
	.create		= guestmemfs_create,
	.lookup		= guestmemfs_lookup,
	.unlink		= guestmemfs_unlink,
};

