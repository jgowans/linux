/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
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

#ifndef PAGE_PINNING_IOMMU_H
#define PAGE_PINNING_IOMMU_H

/* "PPIOMMU" in hex */
#define PPIOMMU_MAGIC_NUMBER 0x554d4d4f495050
#define PPIOMMU_DRIVER_VERSION 1

struct ppiommu_hw_interface {
	uint64_t magic_number;
	uint32_t device_version;
	uint32_t driver_version;
	uint64_t refcount_root_phys;
};

/*
 * #defines for aiding slicing up the PFN into indexes for the various levels
 * of the reference counter tree.
 */
#define PPIOMMU_PFN_TO_HUGE_PAGE_SHIFT 9
#define PPIOMMU_LEAF_PAGE_BITS 11
#define PPIOMMU_SECOND_LVL_BITS 9
#define PPIOMMU_FIRST_LVL_BITS 9
#define PPIOMMU_MAX_PFN_BITS (PPIOMMU_PFN_TO_HUGE_PAGE_SHIFT + \
		PPIOMMU_LEAF_PAGE_BITS + PPIOMMU_SECOND_LVL_BITS + \
		PPIOMMU_FIRST_LVL_BITS)

/*
 * Once a reference counter hits this value the page will be permanently
 * pinned.
 */
#define PPIOMMU_REFCOUNTER_MAX 65535

#endif
