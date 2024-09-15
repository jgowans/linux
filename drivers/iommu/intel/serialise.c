// SPDX-License-Identifier: GPL-2.0-only

#include "iommu.h"

static int serialise_domain(void *fdt, struct iommu_domain *domain)
{
	return 0;
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

int __init intel_iommu_deserialise_kho(void)
{
	return 0;
}
