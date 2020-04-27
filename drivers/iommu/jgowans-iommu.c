#include <linux/bits.h>
#include <linux/bitops.h>
#include <linux/printk.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/device.h>
#include <linux/pfn.h>
#include <linux/scatterlist.h>
#include <linux/module.h>


// Each element is a pointer to the phys addr of the next level.
// Forcing each element to be 64-bit value so hypervisor can index
// into it. Investigate using arch-specific sizes.
static uint64_t *bitmap_first_lvl;

static atomic_t pages_allocated = ATOMIC_INIT(0);
module_param_named(pages_allocated, pages_allocated.counter, int, 0664);

static atomic_t dma_handles = ATOMIC_INIT(0);
module_param_named(dma_handles, dma_handles.counter, int, 0664);

static atomic_t pinned_pages = ATOMIC_INIT(0);
module_param_named(pinned_pages, pinned_pages.counter, int, 0664);

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

static void *account_get_page(void) {
	atomic_inc(&pages_allocated);
	return (void *)get_zeroed_page(GFP_KERNEL);
}

// TODO: global spin lock for mutations. Mutations should be rare.
// I feel like I'm re-implementing page table walking here.... I wonder if there are
// some helper methods to do this heavy lifting for me?
static void *refcounter_page(unsigned long pfn) {
	unsigned int first_lvl_idx, second_lvl_idx;
	uint64_t second_lvl_phys, leaf_phys; // use phys_addr_t?
	uint64_t *second_lvl;
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
		second_lvl_phys = virt_to_phys(account_get_page()); // may be mis-match on 32-bit.
		bitmap_first_lvl[first_lvl_idx] = second_lvl_phys;
	}
	second_lvl = (uint64_t *)phys_to_virt(second_lvl_phys);
	leaf_phys = second_lvl[second_lvl_idx];
	if (!leaf_phys) {
		printk("allocating a leaf page\n");
		// lock
		// re-check
		leaf_phys = virt_to_phys(account_get_page());
		second_lvl[second_lvl_idx] = leaf_phys;
		// unlock
	}
	return phys_to_virt(leaf_phys);
}

static void mark_pfns_in_use(unsigned long pfn, int n) {
	int counter_idx;
	unsigned char *addr; // hard-coding to 8-bit here. This would need to be fancier.
	unsigned char old, new;
	//printk("making in use: 0x%lx\n", pfn);
	//printk("n: %i\n", n);
	//dump_stack();
	// It may seem inefficient to do this one. bit. at. a. time.
	// But actually we typically only have a single bit.
	while(n) { 
		//atomic_inc_and_test(&val);
		//cmpxchg
		counter_idx = pfn & ((1 << LEAF_PAGE_BITS) - 1);
		addr = refcounter_page(pfn) + counter_idx;
		//printk("bit idx: 0x%x\n", bit_idx);
		//old_val = test_and_set_bit(bit_idx, bitmap_page(pfn));
		do {
			old = *addr;
			BUG_ON(old > 127);
			new = old + 1;
		} while (cmpxchg(addr, old, new) != old);
		atomic_inc(&dma_handles);
		if (new == 1)
			atomic_inc(&pinned_pages);
		n--; pfn++;
	}

}

static void mark_pfns_free(uint64_t pfn, int n) {
	int counter_idx;
	unsigned char *addr; // hard-coding to 8-bit here. This would need to be fancier.
	unsigned char old, new;
	//printk("marking free: 0x%lx\n", pfn);
	//printk("n: %i\n", n);
	//dump_stack();
	// It may seem inefficient to do this one. bit. at. a. time.
	// But actually we typically only have a single bit.
	while(n) { 
		//atomic_inc_and_test(&val);
		//cmpxchg
		counter_idx = pfn & ((1 << LEAF_PAGE_BITS) - 1);
		addr = refcounter_page(pfn) + counter_idx;
		//printk("bit idx: 0x%x\n", bit_idx);
		//old_val = test_and_set_bit(bit_idx, bitmap_page(pfn));
		do {
			old = *addr;
			BUG_ON(old < 1);
			new = old - 1;
		} while (cmpxchg(addr, old, new) != old);
		atomic_dec(&dma_handles);
		if (new == 0)
			atomic_dec(&pinned_pages);
		n--; pfn++;
	}
}

#if 0
static void mark_lots_of_pfns_free(uint64_t pfn, int n) {
	while(n) { // loops for each bitmap page
		page_ptr = page_for_pfn(pfn);
		start_idx = pfn & ((1 << LEAF_PAGE_BITS) - 1);
		end_idx = min(n, (1 << LEAF_PAGE_BITS));
		bits_to_process = end_idx - start_idx;
		set_bits(page_ptr, start_idx, bits_to_process);
		pfn += bits_to_process;
		n -= bits_to_process;
	}
}
#endif

static void *jg_alloc(struct device *dev, size_t size,
			dma_addr_t *dma_handle, gfp_t gfp,
			unsigned long attrs) {
	void *kaddr;
	//printk("jgowans in alloc with size 0x%lx\n", size);
	// It's a bit tricky to follow with all of the CONFIG_ switches here and
	// in the called functions, but I *think* that the amount of data which is
	// actually allocated is always aligned to a page. So there is no possibility
	// that the first invocation will alloc the first half of a page, and the
	// second invocation will alloc the second half. This means that we should 
	// never get two pointers which map to the same PFN returned. Hence it's a
	// BUG if a PFN which is already marked for DMA is marked again.
	kaddr = dma_direct_alloc(dev, size, dma_handle, gfp, attrs);
	printk("Doing vmalloc_to_pfn for %px\n", kaddr);
	mark_pfns_in_use(PHYS_PFN(virt_to_phys(kaddr)), 1);
	return kaddr;
}

static void jg_free(struct device *dev, size_t size,
		      void *vaddr, dma_addr_t dma_handle,
		      unsigned long attrs) {
	printk("jgowans in free with size 0x%lx\n", size);
	//uint32_t pages = 1 << get_order(size);
	dma_direct_free(dev, size, vaddr, dma_handle, attrs);
}
static int jg_mmap(struct device *dev, struct vm_area_struct *vma,
		  void *cpu_addr, dma_addr_t dma_addr, size_t size,
		  unsigned long attrs) {
	printk("jgowans in mmap\n");
	mark_pfns_free(PHYS_PFN(virt_to_phys(cpu_addr)), 1);
	return 0;
}

static dma_addr_t jg_map_page(struct device *dev, struct page *page,
		       unsigned long offset, size_t size,
		       enum dma_data_direction dir,
		       unsigned long attrs) {
	int start_pfn_offset, end_pfn_offset_excl;
	//printk("jgowans in map_page offset 0x%lx size 0x%lx\n", offset, size);
	start_pfn_offset = PFN_DOWN(offset);
	end_pfn_offset_excl = PFN_UP(offset + size);
	//printk("start_pfn_offset: 0x%lx end_pfn_offset 0x%lx\n",
	//		start_pfn_offset, end_pfn_offset_excl);
	mark_pfns_in_use(
			page_to_pfn(page) + start_pfn_offset,
			end_pfn_offset_excl - start_pfn_offset);
	return dma_direct_map_page(dev, page, offset, size, dir, attrs);
}
static void jg_unmap_page(struct device *dev, dma_addr_t dma_handle,
		   size_t size, enum dma_data_direction dir,
		   unsigned long attrs) {
	unsigned long start_pfn, end_pfn_excl;
	start_pfn = PFN_DOWN(dma_handle);
	end_pfn_excl = PFN_UP(dma_handle + size);
	//printk("jgowans in unmap_page handle 0x%lx size 0x%lx\n", dma_handle, size);
	//printk("start_pfn: 0x%lx end_pfn: 0x%lx\n", start_pfn, end_pfn_excl);
	mark_pfns_free(start_pfn, end_pfn_excl - start_pfn);
	return dma_direct_unmap_page(dev, dma_handle, size, dir, attrs);
}
/*
 * map_sg returns 0 on error and a value > 0 on success.
 * It should never return a value < 0.
 */
static int jg_map_sg(struct device *dev, struct scatterlist *sg,
	      int nents, enum dma_data_direction dir,
	      unsigned long attrs) {
	printk("jgowans in map_sg\n");
	return dma_direct_map_sg(dev, sg, nents, dir, attrs);
	//return nents;
}
static void jg_unmap_sg(struct device *dev,
		 struct scatterlist *sg, int nents,
		 enum dma_data_direction dir,
		 unsigned long attrs) {
	//printk("jgowans in unmap_sg\n");
	dma_direct_unmap_sg(dev, sg, nents, dir, attrs);
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
