// SPDX-License-Identifier: GPL-2.0-only

#include "iommu.h"

/*
 * Serialised format:
 * /intel-iommu
 *     compatible = str
 *     domains = {
 *         persistent-id = {
 *             mem = [ ... ] // page table pages
 *             agaw = i32
 *             pgd = u64
 *             devices = {
 *                 id = {
 *                     u8 bus;
 *                     u8 devfn
 *                 },
 *                 ...
 *             }
 *         }
 *      }
 */

/*
 * Adds all present PFNs on the PTE page to the kho_mem pointer and advances
 * the pointer.
 * Stolen from dma_pte_list_pagetables() */
static void save_pte_pages(struct dmar_domain *domain, int level,
			   struct dma_pte *pte, struct kho_mem **kho_mem)
{
	struct page *pg;

	pg = pfn_to_page(dma_pte_addr(pte) >> PAGE_SHIFT);
	
	if (level == 1)
		return;

	pte = page_address(pg);
	do {
		if (dma_pte_present(pte)) {
			(*kho_mem)->addr = dma_pte_addr(pte);
			(*kho_mem)->len = PAGE_SIZE;
			(*kho_mem)++;
			if (!dma_pte_superpage(pte))
				save_pte_pages(domain, level - 1, pte, kho_mem);
		}
		pte++;
	} while (!first_pte_in_page(pte));
}
		
static int serialise_domain(void *fdt, struct iommu_domain *domain)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);
	/*
	 * kho_mems_start points to the original allocated array; kho_mems
	 * is incremented by the callee. Keep both to know how many were added.
	 */
	struct kho_mem *kho_mems, *kho_mems_start;
	struct device_domain_info *info;
	int err = 0;
	char name[24];
	int device_idx = 0;
	phys_addr_t pgd;

	/*
	 * Assume just one page worth of kho_mem objects is enough.
	 * Better would be to keep track of number of allocated pages in the domain.
	 * */
	kho_mems_start = kho_mems = kzalloc(PAGE_SIZE, GFP_KERNEL);

	save_pte_pages(dmar_domain, agaw_to_level(dmar_domain->agaw),
		       dmar_domain->pgd, &kho_mems);

	snprintf(name, sizeof(name), "%lu", domain->persistent_id);
	err |= fdt_begin_node(fdt, name);
	err |= fdt_property(fdt, "mem", kho_mems_start,
			sizeof(struct kho_mem) * (kho_mems - kho_mems_start));
	err |= fdt_property(fdt, "persistent_id", &domain->persistent_id,
			sizeof(domain->persistent_id));
	pgd = virt_to_phys(dmar_domain->pgd);
	err |= fdt_property(fdt, "pgd", &pgd, sizeof(pgd));
	err |= fdt_property(fdt, "agaw", &dmar_domain->agaw,
			sizeof(dmar_domain->agaw));

	err |= fdt_begin_node(fdt, "devices");
	list_for_each_entry(info, &dmar_domain->devices, link) {
		snprintf(name, sizeof(name), "%i", device_idx++);
		err |= fdt_begin_node(fdt, name);
		err |= fdt_property(fdt, "bus", &info->bus, sizeof(info->bus));
		err |= fdt_property(fdt, "devfn", &info->devfn, sizeof(info->devfn));
		err |= fdt_end_node(fdt); /* device_idx */
	}
	err |= fdt_end_node(fdt); /* devices */
	err |= fdt_end_node(fdt); /* domain->persistent_id */

	return err;
}

int intel_iommu_serialise_kho(struct notifier_block *self, unsigned long cmd,
			  void *fdt)
{
	static const char compatible[] = "intel-iommu-v0";
	struct iommu_domain *domain;
	unsigned long xa_idx;
	int err = 0;

	switch (cmd) {
	case KEXEC_KHO_ABORT:
		/* Would do serialise rollback here. */
		return NOTIFY_DONE;
	case KEXEC_KHO_DUMP:
		err |= fdt_begin_node(fdt, "intel-iommu");
		fdt_property(fdt, "compatible", compatible, sizeof(compatible));
		err |= fdt_begin_node(fdt, "domains");
		xa_for_each(&persistent_domains, xa_idx, domain) {
			err |= serialise_domain(fdt, domain);
		}
		err |= fdt_end_node(fdt); /* domains */
		err |= fdt_end_node(fdt); /* intel-iommu*/
		return err? NOTIFY_BAD : NOTIFY_DONE;
	default:
		return NOTIFY_BAD;
	}
}

static void deserialise_domains(const void *fdt, int root_off)
{
	int off;
	struct dmar_domain *dmar_domain;

	fdt_for_each_subnode(off, fdt, root_off) {
		const struct kho_mem *kho_mems;
		int len, idx;
		const unsigned long *pgd_phys;
		const int *agaw;
		const unsigned long *persistent_id;
		int rc;

		dmar_domain = alloc_domain(IOMMU_DOMAIN_UNMANAGED);

		kho_mems = fdt_getprop(fdt, off, "mem", &len);
		for (idx = 0; idx * sizeof(struct kho_mem) < len; ++idx)
			kho_claim_mem(&kho_mems[idx]);

		pgd_phys = fdt_getprop(fdt, off, "pgd", &len);
		dmar_domain->pgd = phys_to_virt(*pgd_phys);
		agaw = fdt_getprop(fdt, off, "agaw", &len);
		dmar_domain->agaw = *agaw;
		persistent_id = fdt_getprop(fdt, off, "persistent_id", &len);
		dmar_domain->domain.persistent_id = *persistent_id;

		rc = xa_insert(&persistent_domains, *persistent_id,
				&dmar_domain->domain, GFP_KERNEL);
		if (rc)
			pr_warn("Unable to re-insert persistent domain %lu\n", *persistent_id);
	}
}

int __init intel_iommu_deserialise_kho(void)
{
	const void *fdt = kho_get_fdt();
	int off;

	if (!fdt)
		return 0;

	off = fdt_path_offset(fdt, "/intel-iommu");
	if (off <= 0)
		return 0; /* No data in KHO */

	deserialise_domains(fdt, fdt_subnode_offset(fdt, off, "domains"));
	return 0;
}
