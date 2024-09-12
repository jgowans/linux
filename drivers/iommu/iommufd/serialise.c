// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kexec.h>
#include <linux/libfdt.h>
#include "iommufd_private.h"
#include "io_pagetable.h"

/**
 * Serialised format:
 * /iommufd
 *   compatible = "iommufd-v0",
 *   iommufds = [
 *     persistent_id = {
 *       account_mode = u8
 *       ioases = [
 *         {
 *           areas = [
 *           ]
 *         }
 *       ]
 *     }
 *   ]
 */
static int serialise_iommufd(void *fdt, struct iommufd_ctx *ictx)
{
	int err = 0;
	char name[24];
	struct iommufd_object *obj;
	unsigned long obj_idx;

	snprintf(name, sizeof(name), "%lu", ictx->persistent_id);
	err |= fdt_begin_node(fdt, name);
	err |= fdt_begin_node(fdt, "ioases");
	xa_for_each(&ictx->objects, obj_idx, obj) {
		struct iommufd_ioas *ioas;
		struct iopt_area *area;
		int area_idx = 0;

		if (obj->type != IOMMUFD_OBJ_IOAS)
			continue;

		ioas = (struct iommufd_ioas *) obj;
		snprintf(name, sizeof(name), "%lu", obj_idx);
		err |= fdt_begin_node(fdt, name);

		for (area = iopt_area_iter_first(&ioas->iopt, 0, ULONG_MAX); area;
				area = iopt_area_iter_next(area, 0, ULONG_MAX)) {
			unsigned long iova_start, iova_len;

			snprintf(name, sizeof(name), "%i", area_idx);
			err |= fdt_begin_node(fdt, name);
			iova_start = iopt_area_iova(area);
			iova_len = iopt_area_length(area);
			err |= fdt_property(fdt, "iova-start",
					&iova_start, sizeof(iova_start));
			err |= fdt_property(fdt, "iova-len",
					&iova_len, sizeof(iova_len));
			err |= fdt_property(fdt, "iommu-prot",
					&area->iommu_prot, sizeof(area->iommu_prot));
			err |= fdt_end_node(fdt); /* area_idx */
			++area_idx;
		}
		err |= fdt_end_node(fdt); /* ioas obj_idx */
	}
	err |= fdt_end_node(fdt); /* ioases*/
	err |= fdt_end_node(fdt); /* ictx->persistent_id */
	return 0;
}

int iommufd_serialise_kho(struct notifier_block *self, unsigned long cmd,
			  void *fdt)
{
	static const char compatible[] = "iommufd-v0";
	struct iommufd_ctx *ictx;
	unsigned long xa_idx;
	int err = 0;

	switch (cmd) {
	case KEXEC_KHO_ABORT:
		/* Would do serialise rollback here. */
		return NOTIFY_DONE;
	case KEXEC_KHO_DUMP:
		err |= fdt_begin_node(fdt, "iommufd");
		fdt_property(fdt, "compatible", compatible, sizeof(compatible));
		err |= fdt_begin_node(fdt, "iommufds");
		xa_for_each(&persistent_iommufds, xa_idx, ictx) {
			err |= serialise_iommufd(fdt, ictx);
		}
		err |= fdt_end_node(fdt); /* iommufds */
		err |= fdt_end_node(fdt); /* iommufd */
		return err? NOTIFY_BAD : NOTIFY_DONE;
	default:
		return NOTIFY_BAD;
	}
}

int __init iommufd_deserialise_kho(void)
{
	pr_info("would deserialise here\n");
	return 0;
}
