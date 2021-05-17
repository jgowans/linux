// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Copyright 2020 Amazon.com, Inc. or its affiliates.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// #define DEBUG 1

#include <linux/bits.h>
#include <linux/bitops.h>
#include <linux/printk.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/device.h>
#include <linux/pfn.h>
#include <linux/scatterlist.h>
#include <linux/module.h>
#include <linux/bug.h>
#include <linux/random.h>
#include <linux/acpi.h>

#include "page-pinning-iommu.h"

/*
 * Hackidy hack!
 * What's the right way to do this?
 */
#include "../../kernel/dma/direct.h"

static bool ppiommu_force_enable __ro_after_init;
module_param_named(enable, ppiommu_force_enable, bool, 0400);
MODULE_PARM_DESC(ppiommu_force_enable,
		"Enable page touching even if no device detected in ACPI");

static atomic_t pages_allocated = ATOMIC_INIT(0);
module_param_named(pages_allocated, pages_allocated.counter, int, 0664);

/*
 * How many references we've given out. This is likely more than were requested
 * because we pin at huge page granuality but requests come in at page
 * granuality.
 */
static atomic_t dma_handles = ATOMIC_INIT(0);
module_param_named(dma_handles, dma_handles.counter, int, 0664);

static atomic_t pinned_pages = ATOMIC_INIT(0);
module_param_named(pinned_pages, pinned_pages.counter, int, 0664);

static atomic_t permanently_pinned_pages = ATOMIC_INIT(0);
module_param_named(permanently_pinned_pages, permanently_pinned_pages.counter,
		int, 0664);

/*
 * This virtual IOMMU device driver is designed to allow the kernel to
 * communicate with the hypervisor and request that pages be pinned in
 * memory when the kernel is going to use them for DMA. This is necessary
 * in a memory overcommit environment with DMA devices passed through to
 * the guest.
 * Refernece counter are used to track the need for pages to stay resident.
 * A tree of pages of reference counters is maintained by this driver.
 * Each reference counter referres to a 2 MiB huge page. When the reference
 * counter is non-zero it's an instruction to the hypervisor that the page
 * referred to by the reference counter
 * must remain present.
 * DMA reference counter are used for each page. The reference counters
 * are written by this guest driver and ready by the hypervisor virtual
 * Two mechanism are used here:
 * Page tracking is done at 2 MiB huge page granuality.
 *
 * Before handing a page over to another device driver, this vIOMMU driver
 * will increment the reference counter for that page to pin it, and then
 * touch the page to ensure that it's currently resident.
 *
 * This root pointer is a pointer to one page; the root page.
 * This page is an array of pointers to second level pages.
 * Thei second level  page is an array of pointer to leaf pages.
 * The leaf pages is an array of 8-bit ref counters, one counter
 * for each.
 * So the maximum number of pages that can be tracked (on x86?)
 * = first_level_size * second_level_size * counters * page_size
 * = 512 * 512 * 4096 * 4096
 * = 2^42 = 4 TiB
 *
 * Each pointer is a 64-bit *physical address* of the next level so
 * that the hypervisor can walk it as well.
 *
 * A "Huge PFN" is use for indexing into the reference counter bitmap.
 * The Huge PFN is the normal 4 k PFN shifted down by 9 to get PFNs at
 * 2 MiB granuality.
 *
 * To walk the tree, use the following bits of a Huge PFN for indexes:
 * 0..10  counter idx
 * 11..19 second page idx
 * 20..28 first page idx
 *
 *                  (first level)
 * refcount_root -> |----------|    (second level)
 *                  | [20..29] | ->  |----------|
 *                  |          |     |          |      (leaf)
 *                  |----------|     | [11..19] | -> |---------|
 *                                   |----------|    | [0..10] |
 *                                                   |         |
 *                                                   |---------|
 *
 * De-allocation hasn't been implemented, but we *could* use the last
 * bits of the second level pointer array as a count of non-zero refs
 * and when it drops to zero de-allocate the leaf pages. That has the
 * down side where we could be bouncing between allocating and freeing
 * a leaf as buffers are allocated and freed. IMO it's better to
 * accumulate leaves over time. The overhead is small(ish) and can be
 * even smaller if we track at huge page granularity.
 *
 * In the event that an 16 bit counter becomes saturated, the page will be
 * permanantly pinned.
 */
static uint64_t *refcount_root;

// #define SIMULATE_FAILURE

/* The supplied location is a pointer to where we want to write the address of
 * a page. It's typically zero and needs to be assigned to a fresh page, but
 * in a race condition it could have already just been populated. Only assign
 * if it's actually zero.
 * In the page table word, this is like a PMD or PUD.
 *
 * Return zero in success: page successfully populated or already was
 * populated. Return -errno on failure.
 */
static int assign_page(uint64_t *physical_page_address)
{
	uint64_t old_phys, new_phys;
	void *new_virt = (void *)get_zeroed_page(GFP_KERNEL);

	if (!new_virt) {
		pr_warn("PPIOMMU unable to GFP. Counters not beign set.\n");
		return -ENOMEM;
	}
	new_phys = virt_to_phys(new_virt);

	/*
	 * If we get there first the current value will be NULL
	 * and the data will be updated. If another thread beat us,
	 * cmpxchg return the value set by the thread that won.
	 */
	old_phys = cmpxchg(physical_page_address, 0, new_phys);

	if (old_phys) { /* we lost; no biggy */
		free_page((uint64_t)new_virt);
		pr_debug("PPIOMMU page assignment lost the race\n");
	} else { /* the fastest gun in the west */
		atomic_inc(&pages_allocated);
		pr_debug("PPIOMMU assigned 0x%llx at 0x%llx\n", new_phys,
				virt_to_phys(physical_page_address));
	}
	return 0;
}

/*
 * Traverse a tree of pages by physical address, and fills in pages where
 * they are missing along the way. The handling of both physical and virtual
 * addresses simultaniously is necessary as the tree is maintained using
 * physical addresses, but the code here needs virtual addresses to read
 * the data.
 *
 * Returns a pointer to the page of reference counters for the specified pfn.
 * Note that this won't point to the exact refcounter, rather the page of
 * refcounter which the one for this page is on.
 *
 * @pfn [in] : the pfn to get the refcounter for
 * @refcounter [out] : pointer to the 16-bit refcounter for the pfn range which
 *                     includes the supplied pfn
 * @n_pfns [out] : how many pfns the supplied refcounter actually map to.
 *
 * Return 0 if the out params were successfully set, or -ERRNO if they could
 * not be set for ERRNO reason.
 */
static int refcounter_for_pfn(unsigned long pfn, uint16_t **refcounter,
				    int *n_pfns)
{
	unsigned int first_lvl_idx, second_lvl_idx;

	/*
	 * Pointers to the physical addresses of the start of the intermediate
	 * pages. Similar to a *pud_t and *pmd_t.
	 */
	uint64_t *second_lvl_phys_ptr, *leaf_phys_ptr; // use phys_addr_t?
	uint64_t *second_lvl_ptr;
	uint16_t *leaf_ptr;
	uint64_t leaf_offset;

	/* Each refcounter is responsible for one huge page worth of pfns */
	*n_pfns = 1 << PPIOMMU_PFN_TO_HUGE_PAGE_SHIFT;

	if (pfn >= (1UL << PPIOMMU_MAX_PFN_BITS)) {
		pr_warn("PPIOMMU: Invalid huge pfn %lu supplied\n", pfn);
		return -EINVAL;
	}

	/* Pre-compute indexes for clean reading */
	pfn = pfn >> PPIOMMU_PFN_TO_HUGE_PAGE_SHIFT;
	leaf_offset = pfn & ((1 << PPIOMMU_LEAF_PAGE_BITS) - 1);
	pfn = pfn >> PPIOMMU_LEAF_PAGE_BITS;
	second_lvl_idx = pfn & ((1 << PPIOMMU_SECOND_LVL_BITS) - 1);
	pfn = pfn >> PPIOMMU_SECOND_LVL_BITS;
	first_lvl_idx = pfn & ((1 << PPIOMMU_FIRST_LVL_BITS) - 1);

	/*
	 * Root page is an array of pointers to the physical address of the
	 * second level pages.
	 * second_lvl_phys_ptr is the address of the physical address of the
	 * start of the second level page. Consider it a *pmd_t.
	 */
	second_lvl_phys_ptr = &refcount_root[first_lvl_idx];
	if (!*second_lvl_phys_ptr) { /* It's null, we need to allocate */
		pr_debug("Allocating a second level page\n");
		assign_page(second_lvl_phys_ptr);
		if (!*second_lvl_phys_ptr) /* Out of puff. Bail. */
			return -ENOMEM;
	}

	/*
	 * Dereferencing second_lvl_phys_ptr give us the physical addr of the
	 * second level page.
	 */
	second_lvl_ptr = (uint64_t *)phys_to_virt(*second_lvl_phys_ptr);

	/*
	 * Second level page is an array of pointers to the physical address
	 * of a leaf page.
	 */
	leaf_phys_ptr = &second_lvl_ptr[second_lvl_idx];
	if (!*leaf_phys_ptr) {
		pr_debug("Allocating a leaf page\n");
		assign_page(leaf_phys_ptr);
		if (!*leaf_phys_ptr)
			return -ENOMEM;
	}

	/*
	 * Dereferencing leaf_phys_ptr gives us the physical address of the
	 * start of the leaf page of reference counters.
	 */
	leaf_ptr = (uint16_t *)phys_to_virt(*leaf_phys_ptr);
	*refcounter = leaf_ptr + leaf_offset;
	return 0;
}

/*
 * Return the number of pfns marked in use. Hopefully this will be n
 * but it can be lower if we failed due to counter limitations or OOM
 * when allocating a counter page.
 *
 * Note that the returned "pinned" value can and typically will be greater than
 * the requested "n." This is because tracking is done at huge page granuality
 * so even if only requesting 1 pfn be pinned, we will actually pin 512 pfns.
 */
static int mark_pfns_in_use(unsigned long pfn, int n)
{

	uint16_t *refcounter, old_refcount;
	int refcounter_pfns, rc;
	unsigned long last_pinned = pfn;
	int i;
#ifdef SIMULATE_FAILURE
	unsigned int rnd = 0;

	get_random_bytes(&rnd, 2);
	if (rnd > 0xfff0) {
		pr_info("Simulating failure for pfn 0x%lx\n", pfn);
		return n;
	}
#endif

	pr_debug("Marking 0x%lx + 0x%x in use\n", pfn, n);

	/* TODO: How do we ensure we always make forward progress or break? */
	while (last_pinned < pfn + n) {
		rc = refcounter_for_pfn(last_pinned, &refcounter,
				&refcounter_pfns);
		/* Can't get the refcounter; bail */
		if (rc)
			break;

		/* Aligned is what we're actually going to pin now. */
		last_pinned = (last_pinned & ~(refcounter_pfns - 1));

		/*
		 * Is there a simpler replacement using an atomic_* function?
		 * It's important that the upper bound case can be catered for
		 * so we can't just to a blind atomic_inc
		 */
		do {
			old_refcount = *refcounter;
		} while (old_refcount < PPIOMMU_REFCOUNTER_MAX &&
				cmpxchg(refcounter, old_refcount,
					old_refcount + 1) != old_refcount);

		/*
		 * This thread had just pinned a bunch of pfns that were
		 * previously not pinned.
		 * refcounter_pfns is how many pfns the refcounter refers to
		 * or represents. By incrementing the refcounter we've just
		 * pinned that many pfns.
		 * (there's an alignment thing to consider as well...)
		 */
		if (old_refcount == 0) {
			pr_debug("pfns %lx + %i are now pinned at 0x%llx\n",
					last_pinned, refcounter_pfns,
					virt_to_phys(refcounter));
			atomic_add(refcounter_pfns, &pinned_pages);
		}
		atomic_add(refcounter_pfns, &dma_handles);

		/*
		 * If we got here and the old_refcount was one before max,
		 * it's guaranteed that this thread did the increment and
		 * permanently pinned the pfns.
		 */
		if (old_refcount == (PPIOMMU_REFCOUNTER_MAX - 1)) {
			pr_debug("pfns %lx + %i are now permanently pinned\n",
					last_pinned, refcounter_pfns);
			atomic_add(refcounter_pfns, &permanently_pinned_pages);
		}

		last_pinned += refcounter_pfns;
	}
	if (last_pinned < pfn + n)
		pr_warn("PPIOMMU: unable to pin all requested pages\n");

	/*
	 * Read to ensure page is resident right now
	 * TODO: does the cmpxchg provide the necessary memory barrier
	 * to ensure:
	 * 1. the increment happens before the read.
	 * 2. the increment is written back to memory; no caching
	 *
	 * Touch all pfns at 4KB granularity to cope with 4KB mappings host-side
	 * and ensure that we never touch memory outside the DMA buffer itself,
	 * even though we're pinning at 2MB granularity.
	 */
	for (i = 0; i < n; i++) {
		if (pfn_valid(pfn+i)) {
			readl(phys_to_virt(PFN_PHYS(pfn+i)));
		} else {
			pr_warn("readl pfn 0x%lx invalid for pfn range 0x%lx npfns %d\n",
				pfn+i, pfn, n);
		}
	}

	return last_pinned - pfn;
}

static void mark_pfns_free(unsigned long pfn, int n)
{
	uint16_t *refcounter, old_refcount;
	int rc, refcounter_pfns;
	unsigned long last_unpinned = pfn;

	pr_debug("Marking 0x%lx + 0x%x free\n", pfn, n);

	/* TODO: ensure we're always making forward progress or bail */
	while (last_unpinned < pfn + n) {
		rc = refcounter_for_pfn(last_unpinned, &refcounter,
				&refcounter_pfns);
		/*
		 * Can't allocate a refcounter and we were asked to free pfns
		 * that were never marked as in-use in the first space.
		 * This is highly pathalogical....
		 */
		if (rc) {
			WARN(1, "Trying to free PFN %lx was never used\n", pfn);
			/* Nothing else we can do but move on. */
			++last_unpinned;
			continue;
		}

		last_unpinned = (last_unpinned & ~(refcounter_pfns - 1));

		do {
			old_refcount  = *refcounter;
		} while (old_refcount < PPIOMMU_REFCOUNTER_MAX &&
				old_refcount > 0 &&
				cmpxchg(refcounter, old_refcount,
					old_refcount - 1) != old_refcount);

		if (old_refcount == 1) {
			pr_debug("pfns %lx + %i are no longer pinned t 0x%llx\n",
					last_unpinned, refcounter_pfns,
					virt_to_phys(refcounter));
			atomic_sub(refcounter_pfns, &pinned_pages);
		}
		/* Indicative of a double free from a caller. */
		if (unlikely(old_refcount == 0))
			WARN(1, "PPIOMMU PFN 0x%lx already zero\n",
					last_unpinned);
		else
			atomic_sub(refcounter_pfns, &dma_handles);

		last_unpinned += refcounter_pfns;
	}
}

static void *ppiommu_alloc(struct device *dev, size_t size,
			dma_addr_t *dma_handle, gfp_t gfp,
			unsigned long attrs)
{
	int marked = 0;
	// Is this the best way to round up to the number of pages?
	int n_pages = PFN_UP(size);
	void *kaddr = dma_direct_alloc(dev, size, dma_handle, gfp, attrs);

	if (!kaddr)
		return NULL;
	marked = mark_pfns_in_use(PHYS_PFN(virt_to_phys(kaddr)), n_pages);
	if (marked < n_pages) { /* marking failed; rollbck and error */
		pr_warn("PPIOMMU unable to mark alloc'd pages in use\n");
		dma_direct_free(dev, size, kaddr, *dma_handle, attrs);
		mark_pfns_free(PHYS_PFN(virt_to_phys(kaddr)), marked);
		return NULL;
	}
	WARN_ON((unsigned long)kaddr & ~PAGE_MASK);
	return kaddr;
}

static void ppiommu_free(struct device *dev, size_t size,
		      void *vaddr, dma_addr_t dma_handle,
		      unsigned long attrs)
{
	/* Device driver is lying to us... should we by detecting this? */
	WARN_ON(dma_to_phys(dev, dma_handle) != virt_to_phys(vaddr));
	dma_direct_free(dev, size, vaddr, dma_handle, attrs);
	mark_pfns_free(PHYS_PFN(virt_to_phys(vaddr)), PFN_UP(size));
}

static dma_addr_t ppiommu_map_page(struct device *dev, struct page *page,
		       unsigned long offset, size_t size,
		       enum dma_data_direction dir,
		       unsigned long attrs)
{
	unsigned long start_pfn, n_pfns;
	int marked;
	dma_addr_t dma_addr = 0;

	start_pfn = page_to_pfn(page) + PFN_DOWN(offset);
	n_pfns = PFN_UP(offset + size) - PFN_DOWN(offset);
	marked = mark_pfns_in_use(start_pfn, n_pfns);
	if (unlikely(marked < n_pfns))
		goto rollback;
	dma_addr = dma_direct_map_page(dev, page, offset, size, dir, attrs);
	if (unlikely(dma_addr == DMA_MAPPING_ERROR))
		goto rollback;
	return dma_addr;

rollback:
	pr_warn("Page pinning failed; rolling back\n");
	mark_pfns_free(start_pfn, marked);
	return DMA_MAPPING_ERROR;  // TODO: check what callers do with this...

}
static void ppiommu_unmap_page(struct device *dev, dma_addr_t dma_handle,
		   size_t size, enum dma_data_direction dir,
		   unsigned long attrs)
{
	unsigned long start_pfn, n_pfns;
	phys_addr_t phys = dma_to_phys(dev, dma_handle);

	start_pfn = PFN_DOWN(phys);
	n_pfns = PFN_UP(phys + size) - start_pfn;
	mark_pfns_free(start_pfn, n_pfns);
	return dma_direct_unmap_page(dev, dma_handle, size, dir, attrs);
}

/*
 * map_sg returns 0 on error and a value > 0 on success.
 * It should never return a value < 0.
 */
static int ppiommu_map_sg(struct device *dev, struct scatterlist *sglist,
	      int nents, enum dma_data_direction dir,
	      unsigned long attrs)
{
	struct scatterlist *sg;
	int i;
	unsigned long start_pfn_offset, end_pfn_offset_excl;

	/* TODO handle failure and rollback case */
	for_each_sg(sglist, sg, nents, i) {
		start_pfn_offset = PFN_DOWN(sg->offset);
		end_pfn_offset_excl = PFN_UP(sg->offset + sg->length);
		mark_pfns_in_use(page_to_pfn(sg_page(sg)) + start_pfn_offset,
				end_pfn_offset_excl - start_pfn_offset);
	}
	return dma_direct_map_sg(dev, sglist, nents, dir, attrs);
}
static void ppiommu_unmap_sg(struct device *dev,
		 struct scatterlist *sglist, int nents,
		 enum dma_data_direction dir,
		 unsigned long attrs)
{
	struct scatterlist *sg;
	int i;
	unsigned long start_pfn_offset, end_pfn_offset_excl;

	for_each_sg(sglist, sg, nents, i) {
		start_pfn_offset = PFN_DOWN(sg->offset);
		end_pfn_offset_excl = PFN_UP(sg->offset + sg->length);
		mark_pfns_free(page_to_pfn(sg_page(sg)) + start_pfn_offset,
				end_pfn_offset_excl - start_pfn_offset);
	}
	dma_direct_unmap_sg(dev, sglist, nents, dir, attrs);
}

const struct dma_map_ops ppiommu_dma_ops = {
	.alloc			= ppiommu_alloc,
	.free			= ppiommu_free,
	.mmap			= dma_common_mmap,
	.map_page		= ppiommu_map_page,
	.unmap_page		= ppiommu_unmap_page,
	.map_sg			= ppiommu_map_sg,
	.unmap_sg		= ppiommu_unmap_sg,
	.dma_supported		= dma_direct_supported,
};
EXPORT_SYMBOL(ppiommu_dma_ops);

static int __init page_pinning_iommu_init(void)
{

	if (!ppiommu_force_enable)
		return 0;

	pr_info("Page pinning IOMMU is enabled");
	refcount_root = (uint64_t *)get_zeroed_page(GFP_KERNEL);

	/*
	 * It may be preferable to use the struct device specific dma_map_ops
	 * rather than the global dma_ops. However, the link between the pci
	 * bus and the dma_ops seems to be through a struct iommu_ops, which
	 * we're not using here. So there doesn't seem to be a clean way to
	 * attach this dma_map_ops with the devices dynamically.
	 * If there is a way to avoid the global dma_ops, let me know...
	 */
	dma_ops = &ppiommu_dma_ops;
	return 0;
}

/* Must execute after PCI subsystem */
fs_initcall(page_pinning_iommu_init);
