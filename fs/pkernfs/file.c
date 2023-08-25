// SPDX-License-Identifier: GPL-2.0-only

#include "pkernfs.h"
#include <linux/mm.h>

static int truncate(struct inode *inode, loff_t newsize)
{
	unsigned long free_block;
	struct pkernfs_inode *pkernfs_inode;
	unsigned long *mappings;

	pkernfs_inode = pkernfs_get_persisted_inode(inode->i_sb, inode->i_ino);
	mappings = (unsigned long *)pkernfs_addr_for_block(inode->i_sb,
		pkernfs_inode->mappings_block);
	i_size_write(inode, newsize);
	for (int block_idx = 0; block_idx * PMD_SIZE < newsize; ++block_idx) {
		free_block = pkernfs_alloc_block(inode->i_sb);
		if (free_block <= 0)
			/* TODO: roll back allocations. */
			return -ENOMEM;
		*(mappings + block_idx) = free_block;
		++pkernfs_inode->num_mappings;
	}
	return 0;
}

static int inode_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = dentry->d_inode;
	int error;

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

/*
 * To be able to use PFNMAP VMAs for VFIO DMA mapping we need the page tables
 * populated with mappings. Pre-fault everything.
 */
static int mmap(struct file *filp, struct vm_area_struct *vma)
{
	int rc;
	unsigned long *mappings_block;
	struct pkernfs_inode *pkernfs_inode;

	pkernfs_inode = pkernfs_get_persisted_inode(filp->f_inode->i_sb, filp->f_inode->i_ino);

	mappings_block = (unsigned long *)pkernfs_addr_for_block(filp->f_inode->i_sb,
			pkernfs_inode->mappings_block);

	/* Remap-pfn-range will mark the range VM_IO */
	for (unsigned long vma_addr_offset = vma->vm_start;
			vma_addr_offset < vma->vm_end;
			vma_addr_offset += PMD_SIZE) {
		int block, mapped_block;

		block = (vma_addr_offset - vma->vm_start) / PMD_SIZE;
		mapped_block = *(mappings_block + block);
		/*
		 * It's wrong to use rempa_pfn_range; this will install PTE-level entries.
		 * The whole point of 2 MiB allocs is to improve TLB perf!
		 * We should use something like mm/huge_memory.c#insert_pfn_pmd
		 * but that is currently static.
		 * TODO: figure out the best way to install PMDs.
		 */
		rc = remap_pfn_range(vma,
				vma_addr_offset,
				(pkernfs_base >> PAGE_SHIFT) + (mapped_block * 512),
				PMD_SIZE,
				vma->vm_page_prot);
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
