// SPDX-License-Identifier: GPL-2.0-only

#include "pkernfs.h"
#include <linux/mm.h>

/* Duplicated; factor out. */
struct kvm_gmem {
	struct kvm *kvm;
	struct xarray bindings;
	struct list_head entry;
};

bool is_pkernfs_file(struct file *filep)
{
    return filep->f_op == &pkernfs_file_fops;
}

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
		pr_warn("mmapping huge pfn: 0x%llx\n", (pkernfs_base >> PAGE_SHIFT) + (mapped_block * 512));
		pr_warn("...at address: 0x%lx\n", vma_addr_offset);
		pr_warn("... vma->vm_page_prot 0x%lx\n", pgprot_val(vma->vm_page_prot));
		rc = remap_pfn_range(vma,
				vma_addr_offset,
				(pkernfs_base >> PAGE_SHIFT) + (mapped_block * 512),
				PMD_SIZE,
				vma->vm_page_prot);
	}
	return 0;
}

int pkernfs_gmem_bind(struct kvm *kvm, struct kvm_memory_slot *slot,
		      struct file *file, loff_t offset)
{
	struct inode *inode;
	loff_t size = slot->npages << PAGE_SHIFT;
	unsigned long start, end;
	struct kvm_gmem *gmem;
	int r = 0;

	printk("pkernfs_gmem_bind\n");

	gmem = file->private_data;
	if (gmem->kvm != kvm) {
		printk("clobbering gmem->kvm\n");
		gmem->kvm = kvm;
	}

	inode = file_inode(file);

	if (offset < 0 || !PAGE_ALIGNED(offset) ||
	    offset + size > i_size_read(inode))
		goto err;

	filemap_invalidate_lock(inode->i_mapping);

	start = offset >> PAGE_SHIFT;
	end = start + slot->npages;

	if (!xa_empty(&gmem->bindings) &&
	    xa_find(&gmem->bindings, &start, end - 1, XA_PRESENT)) {
		filemap_invalidate_unlock(inode->i_mapping);
		goto err;
	}

	/*
	 * No synchronize_rcu() needed, any in-flight readers are guaranteed to
	 * be see either a NULL file or this new file, no need for them to go
	 * away.
	 */
	rcu_assign_pointer(slot->gmem.file, file);
	slot->gmem.pgoff = start;

	xa_store_range(&gmem->bindings, start, end - 1, slot, GFP_KERNEL);
	filemap_invalidate_unlock(inode->i_mapping);
err:
	return r;
}


int pkernfs_get_pfn(struct file *file, pgoff_t index,
		    kvm_pfn_t *pfn, int *max_order)
{
	unsigned long *mappings_block;
	struct pkernfs_inode *pkernfs_inode;
	unsigned long huge_pfn;
	int mapped_block;

	printk("pkernfs_get_pfn\n");

	pkernfs_inode = pkernfs_get_persisted_inode(file->f_inode->i_sb, file->f_inode->i_ino);
	mappings_block = (unsigned long *)pkernfs_addr_for_block(file->f_inode->i_sb,
			pkernfs_inode->mappings_block);

	mapped_block = *(mappings_block + (index / 512));
	huge_pfn = (pkernfs_base >> PAGE_SHIFT) + (mapped_block * 512);
	*pfn = huge_pfn + (index % 512);
	printk("pfn: 0x%llx\n", *pfn);
	if (max_order)
		*max_order = 0;
	return 0;
}

static int open(struct inode *inode, struct file *file) {
	printk("pkernfs open\n");
	file->private_data = kzalloc(sizeof(struct kvm_gmem), GFP_KERNEL);
	return 0;
}

const struct inode_operations pkernfs_file_inode_operations = {
	.setattr = inode_setattr,
	.getattr = simple_getattr,
};

const struct file_operations pkernfs_file_fops = {
	.owner = THIS_MODULE,
	.mmap = mmap,
	.open = open,
};
