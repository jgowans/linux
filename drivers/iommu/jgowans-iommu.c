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

static atomic_t pages_allocated = ATOMIC_INIT(0);
module_param_named(pages_allocated, pages_allocated.counter, int, 0664);

static atomic_t dma_handles = ATOMIC_INIT(0);
module_param_named(dma_handles, dma_handles.counter, int, 0664);

static atomic_t pinned_pages = ATOMIC_INIT(0);
module_param_named(pinned_pages, pinned_pages.counter, int, 0664);

// Each element is a pointer to the phys addr of the next level.
// Forcing each element to be 64-bit value so hypervisor can index
// into it. Investigate using arch-specific sizes.
static uint64_t *bitmap_first_lvl;
/*
 * This is a pointer to a one page.
 * The page is an array of pointer to other pages.
 * Each page is a bitmap, one it per page.
 *
 * EDIT: multiple skbuffers can be on one page, so we have multiple
 * bits per page. Not sure exactly how many... Putting it in a #define.
 *
 * That means the leaf page holds bits for 4096 * 8 pfns.
 * For x86 where the page is 4 KiB, to go from a PFN to the bit:
 * (ptr[pfn >> 15][pfn >> 1 & 0xFFFF] > (pfn & 0xF)) & 1
 *
 * So the maximum number of pages that can be tracked is
 *
 * Format: [a_start, a_end, a_ptr, b_start, b_end, b_ptr, ...]
 * pfn bits:
 * 0..14  bit idx
 * 15..23 second page idx
 * 24..32 first page idx
 *
 * bitmap_first_lvl -> |----------|
 *                     | [21..29] | ->  |----------|
 *                     |          |     |          |
 *                     |----------|     | [12..20] | -> |---------|
 *                                      |----------|    | [0..11] |
 *                                                      |         |
 *                                                      |---------|
 *
 * Note that the addresses in root pointer, first translation, and
 * second translation laters are physical addresses to allow the 
 * IOMMU device to traverse it. The same as a page table.
 *
 * In x86 this allows 512 * 512 * 4096 * 8 PFNs which is 32 TiB of RAM.
 */

// Should I be using ARCH-specific values here; or stick with hard-coded
// 4 KiB frame sizes so that the IOMMU device code can be arch-independent?
// TODO: do huge page tracking. Muck better optimization for 16-bit counter
// for a 2 MiB page.
#define REFCOUNT_BITS 8
#define LEAF_PAGE_BITS 12
#define SECOND_LVL_BITS 9
#define SECOND_LVL_OFFSET LEAF_PAGE_BITS
#define FIRST_LVL_BITS 9
#define FIRST_LVL_OFFSET (SECOND_LVL_OFFSET + SECOND_LVL_BITS)
#define MAX_PFN_BITS (FIRST_LVL_OFFSET + FIRST_LVL_BITS)

// De-allocation hasn't been implemented, but we *could* use the last
// bits of the second level pointer array as a count of non-zero refs
// and when it drops to zero de-allocate the leaf pages. That has the
// down side where we could be bouncing between allocating and freeing
// a leaf as buffers are allocated and freed. IMO it's better to
// accumulate leaves over time. The overhead is small(ish) and can be
// even smaller if we track at huge page granularity.
static void *account_get_page(void) {
	void *ptr = (void *)get_zeroed_page(GFP_KERNEL); // other GFP flgs? Why does this return ulong?
	if (ptr)
		atomic_inc(&pages_allocated);
	return ptr;
}

// TODO: global spin lock for mutations. Mutations should be rare.
// I feel like I'm re-implementing page table walking here.... I wonder if there are
// some helper methods to do this heavy lifting for me?
static void *refcounter_page(unsigned long pfn) {
	unsigned int first_lvl_idx, second_lvl_idx;
	uint64_t second_lvl_phys, leaf_phys; // use phys_addr_t?
	uint64_t *second_lvl;
	void *scratch;
	pfn_valid(pfn);
	BUG_ON(pfn >= (1UL << MAX_PFN_BITS));

	// Pre-compute indexes for clean reading
	pfn = pfn >> LEAF_PAGE_BITS;
	second_lvl_idx = pfn & ((1 << SECOND_LVL_BITS) - 1);
	pfn = pfn >> SECOND_LVL_BITS;
	first_lvl_idx = pfn & ((1 << FIRST_LVL_BITS) - 1);

	second_lvl_phys = bitmap_first_lvl[first_lvl_idx];
	if (!second_lvl_phys) {
		printk("allocating a second level page\n");
		scratch = account_get_page();
		if (!scratch)
			return NULL;
		second_lvl_phys = virt_to_phys(scratch); // may be mis-match on 32-bit.
		bitmap_first_lvl[first_lvl_idx] = second_lvl_phys;
	}
	second_lvl = (uint64_t *)phys_to_virt(second_lvl_phys);
	leaf_phys = second_lvl[second_lvl_idx];
	if (!leaf_phys) {
		printk("allocating a leaf page\n");
		// lock
		// re-check
		scratch = account_get_page();
		if (!scratch)
			return NULL;
		leaf_phys = virt_to_phys(account_get_page());
		second_lvl[second_lvl_idx] = leaf_phys;
		// unlock
	}
	return phys_to_virt(leaf_phys);
}

// Return the number of pfns marked in use. Hopefully this will be n
// but it can be lower if we failed due to counter limitations or OOM
// when allocating a counter page.
static int mark_pfns_in_use(unsigned long pfn, int n) { // TODO: make this start_pfn and increment a variable.
	unsigned long done = 0;
	unsigned char *addr; // hard-coding to 8-bit here. This would need to be fancier.
	unsigned char old;
	while(done < n) { 
		addr = refcounter_page(pfn + done);
		if (!addr) // OOM.
			break;
		addr += ((pfn + done) & ((1 << LEAF_PAGE_BITS) - 1));
		do {
			old = *addr;
		} while (old < 127 && cmpxchg(addr, old, old + 1) != old);
		if (old >= 127) {
			WARN(1, "Too many references to a single pfn: 0x%lx\n", pfn + done);
			break; // bail. Don't do more work so we can rollback existing work.
		}
		atomic_inc(&dma_handles);
		if (old == 0)
			atomic_inc(&pinned_pages);
		done++;
	}
	return done;

}

static void mark_pfns_free(unsigned long pfn, int n) {
	unsigned char *addr; // hard-coding to 8-bit here. This would need to be fancier.
	unsigned char old;
	int done = 0;
	while(done < n) { 
		addr = refcounter_page(pfn + done);
		if (!addr) {
			WARN(1, "Asked to free a PFN that was never allocated\n");
			continue; // bail rather?
		}
		addr += (pfn + done) & ((1 << LEAF_PAGE_BITS) - 1);
		do {
			old = *addr;
		} while (old > 0 && cmpxchg(addr, old, old - 1) != old);
		if (unlikely(old == 0)) {
			WARN(1, "Reference counter for 0x%lx already zero\n", pfn); // BUG()?
		} else {
			atomic_dec(&dma_handles);
			if (old == 1)
				atomic_dec(&pinned_pages);
		}
		done++; // Optimize re-use of leaf page if only leaf bits changed.
	}
}

static void *jg_alloc(struct device *dev, size_t size,
			dma_addr_t *dma_handle, gfp_t gfp,
			unsigned long attrs) {
	int marked = 0;
	int n_pages = PFN_UP(size); // best way to round up to the number of pages?
	void *kaddr = dma_direct_alloc(dev, size, dma_handle, gfp, attrs);
	BUG_ON(virt_to_phys(kaddr) != dma_to_phys(dev, *dma_handle)); // just checking how this works...
	if (!kaddr)
		return NULL;
	marked = mark_pfns_in_use(PHYS_PFN(virt_to_phys(kaddr)), n_pages);
	if (marked != n_pages) {
		dma_direct_free(dev, size, kaddr, *dma_handle, attrs);
		mark_pfns_free(PHYS_PFN(virt_to_phys(kaddr)), marked);
		return NULL;
	}
	printk("jgalloc returning: 0x%px for size 0x%lx\n", kaddr, size);
	BUG_ON((unsigned long)kaddr & ~PAGE_MASK); // not sure if this is possible?
	return kaddr;
}

static void jg_free(struct device *dev, size_t size,
		      void *vaddr, dma_addr_t dma_handle,
		      unsigned long attrs) {
	//printk("jgowans in free with size 0x%lx\n", size);
	//uint32_t pages = 1 << get_order(size);
	unsigned long start_pfn, offset; //, end_pfn_excl;
	BUG_ON(dma_to_phys(dev, dma_handle) != virt_to_phys(vaddr));
	BUG_ON((unsigned long)vaddr & ~PAGE_MASK); // not sure if this is possible?
	dma_direct_free(dev, size, vaddr, dma_handle, attrs);
	mark_pfns_free(PHYS_PFN(virt_to_phys(vaddr)), PFN_UP(size));
}

static dma_addr_t jg_map_page(struct device *dev, struct page *page,
		       unsigned long offset, size_t size,
		       enum dma_data_direction dir,
		       unsigned long attrs) {
	unsigned long start_pfn, n_pfns;
	int marked;
	dma_addr_t dma_addr = 0;
	start_pfn = page_to_pfn(page) + PFN_DOWN(offset);
	n_pfns = PFN_UP(offset + size) - PFN_DOWN(offset);
	marked = mark_pfns_in_use(start_pfn, n_pfns);
	if (unlikely(marked != n_pfns))
		goto rollback;
	dma_addr = dma_direct_map_page(dev, page, offset, size, dir, attrs);
	if (unlikely(dma_addr == DMA_MAPPING_ERROR))
		goto rollback;
	return dma_addr;

rollback:
	mark_pfns_free(start_pfn, marked);
	return DMA_MAPPING_ERROR;  // TODO: check what callers do with this...

}
static void jg_unmap_page(struct device *dev, dma_addr_t dma_handle,
		   size_t size, enum dma_data_direction dir,
		   unsigned long attrs) {
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
static int jg_map_sg(struct device *dev, struct scatterlist *sglist,
	      int nents, enum dma_data_direction dir,
	      unsigned long attrs) {
	struct scatterlist *sg;
	int i;
	unsigned long start_pfn_offset, end_pfn_offset_excl;
	for_each_sg(sglist, sg, nents, i) {
		//printk("pfn: 0x%lx\n", page_to_pfn(sg_page(sg)));
		start_pfn_offset = PFN_DOWN(sg->offset);
		end_pfn_offset_excl = PFN_UP(sg->offset + sg->length);
		mark_pfns_in_use(
				page_to_pfn(sg_page(sg)) + start_pfn_offset,
				end_pfn_offset_excl - start_pfn_offset);
	}
	return dma_direct_map_sg(dev, sglist, nents, dir, attrs);
	//return nents;
}
static void jg_unmap_sg(struct device *dev,
		 struct scatterlist *sglist, int nents,
		 enum dma_data_direction dir,
		 unsigned long attrs) {
	struct scatterlist *sg;
	int i;
	unsigned long start_pfn_offset, end_pfn_offset_excl;
	for_each_sg(sglist, sg, nents, i) {
		//printk("pfn: 0x%lx\n", page_to_pfn(sg_page(sg)));
		start_pfn_offset = PFN_DOWN(sg->offset);
		end_pfn_offset_excl = PFN_UP(sg->offset + sg->length);
		mark_pfns_free(
				page_to_pfn(sg_page(sg)) + start_pfn_offset,
				end_pfn_offset_excl - start_pfn_offset);
	}
	dma_direct_unmap_sg(dev, sglist, nents, dir, attrs);
}

const struct dma_map_ops jg_dma_ops = {
	.alloc			= jg_alloc,
	.free			= jg_free,
	.mmap			= dma_common_mmap,
	.map_page		= jg_map_page,
	.unmap_page		= jg_unmap_page,
	.map_sg			= jg_map_sg,
	.unmap_sg		= jg_unmap_sg,
	.dma_supported = dma_direct_supported,
};
EXPORT_SYMBOL(jg_dma_ops);

static int __init jgowans_iommu_init(void) {
	uint64_t __iomem *iommu_hw_ptr = ioremap(0xfec10000, 64);
	printk("jgowans got hw ptr: %px\n", iommu_hw_ptr);
	printk("hello darkness my old friend\n");
	bitmap_first_lvl = (uint64_t *)get_zeroed_page(GFP_KERNEL); // why does this return ulong?
	printk("got zero page %px\n", bitmap_first_lvl);
	writeq(virt_to_phys(bitmap_first_lvl), iommu_hw_ptr);
	iounmap(iommu_hw_ptr);
	printk("setting pa: 0x%llx\n", virt_to_phys(bitmap_first_lvl));
	dma_ops = &jg_dma_ops;

	return 0;
}

/* Must execute after PCI subsystem */
fs_initcall(jgowans_iommu_init);
