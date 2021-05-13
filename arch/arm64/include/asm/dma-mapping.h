/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM64_DMA_MAPPING_H
#define _ASM_ARM64_DMA_MAPPING_H

/*
 * Doing the same thing that x86 does so that the page pinning IOMMU driver
 * can set this global DMA ops.
 */

extern const struct dma_map_ops *dma_ops;

static inline const struct dma_map_ops *get_arch_dma_ops(struct bus_type *bus)
{
	return dma_ops;
}

#endif
