// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kexec.h>
#include "iommufd_private.h"

int iommufd_serialise_kho(struct notifier_block *self, unsigned long cmd,
			  void *fdt)
{
	pr_info("would serialise here\n");
	switch (cmd) {
	case KEXEC_KHO_ABORT:
		/* Would do serialise rollback here. */
		return NOTIFY_DONE;
	case KEXEC_KHO_DUMP:
		/* Would do serialise here. */
		return NOTIFY_DONE;
	default:
		return NOTIFY_BAD;
	}
}

int __init iommufd_deserialise_kho(void)
{
	pr_info("would deserialise here\n");
	return 0;
}
