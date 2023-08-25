/* SPDX-License-Identifier: MIT */

#include "pkernfs.h"
#include <linux/mm.h>

static int truncate(struct inode *inode, loff_t newsize)
{
	unsigned long free_block;
	struct pkernfs_inode *pkernfs_inode;
	printk("pkernfs_truncate invoked with size %llu\n", newsize);
	pkernfs_inode = pkernfs_get_persisted_inode(inode->i_sb, inode->i_ino);
	printk("mappings block is: %i\n", pkernfs_inode->mappings_block);
	printk("mappings block vaddr is %px\n",
		pkernfs_addr_for_block(inode->i_sb, pkernfs_inode->mappings_block));
	i_size_write(inode, newsize);
	for (int block_idx = 0; (block_idx * (2 << 20)) < newsize; ++block_idx) {
		printk("allocating block %i\n", block_idx);
		free_block = pkernfs_alloc_block(inode->i_sb);
		printk("free block: %lu\n", free_block);
		*((unsigned long *)pkernfs_addr_for_block(inode->i_sb, pkernfs_inode->mappings_block)
			+ block_idx) = free_block;
		printk("Set mapping at %px to %lu\n",
			((unsigned long *)pkernfs_addr_for_block(inode->i_sb, pkernfs_inode->mappings_block) + block_idx),
			free_block);
		++pkernfs_inode->num_mappings;
	}
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

static int mmap(struct file *filp, struct vm_area_struct *vma)
{
	int rc;
	unsigned long *mappings_block;
	struct pkernfs_inode *pkernfs_inode;

	pkernfs_inode = pkernfs_get_persisted_inode(filp->f_inode->i_sb, filp->f_inode->i_ino);

	printk("Mappings block idx is %i\n", pkernfs_inode->mappings_block);
	mappings_block = (unsigned long *)pkernfs_addr_for_block(filp->f_inode->i_sb, pkernfs_inode->mappings_block);
	printk("Mappings block vaddr is %px\n", mappings_block);

	/* Remap-pfn-range will mark the range VM_IO */
	for (unsigned long vma_addr_offset = vma->vm_start; vma_addr_offset < vma->vm_end; vma_addr_offset += (2 << 20)) {
		int block, mapped_block;
		block = (vma_addr_offset - vma->vm_start) / (2 << 20);
		mapped_block = *(mappings_block + block);
		printk("Read mapping at %px\n", (mappings_block + block));
		printk("Setting mapping for block %i to block %i which is PFN %llu\n",
				block, mapped_block,
				(pkernfs_base >> 12) + (mapped_block * 512));
		printk("Remapping vaddr 0x%lx with prot: %lx\n",
			vma->vm_start + vma_addr_offset,
			vma->vm_page_prot.pgprot);

		rc = remap_pfn_range(vma,
				vma_addr_offset,
				(pkernfs_base >> 12) + (mapped_block * 512),
				(2 << 20),
				vma->vm_page_prot);
		printk("remap_pfn_range returned %i\n", rc);
	}
	return 0;
}

const struct inode_operations pkernfs_file_inode_operations = {
	.setattr = inode_setattr,
	.getattr = simple_getattr,
};

const struct file_operations pkernfs_file_fops = {
	.owner = THIS_MODULE,
	.mmap = mmap,
};
