/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/list.h>
#include <linux/nodemask.h>
#include <linux/slab.h>
#include <linux/dmem.h>

struct dmem_mem_node {
	struct list_head node;
};

static LIST_HEAD(dmem_list);

static int dmem_test_alloc_init(unsigned long dpage_shift)
{
	int ret;

	ret = dmem_alloc_init(dpage_shift);
	if (ret)
		pr_info("dmem_alloc_init failed, dpage_shift %ld ret=%d\n",
			dpage_shift, ret);
	return ret;
}

static int __dmem_test_alloc(int order, int nid, nodemask_t *nodemask,
			     const char *caller)
{
	struct dmem_mem_node *pos;
	phys_addr_t addr;
	int i, ret = 0;

	for (i = 0; i < (1 << order); i++) {
		addr = dmem_alloc_pages_nodemask(nid, nodemask, 1, NULL);
		if (!addr) {
			ret = -ENOMEM;
			break;
		}

		pos = __va(addr);
		list_add(&pos->node, &dmem_list);
	}

	pr_info("%s: alloc order %d on node %d has fallback node %s... %s.\n",
		caller, order, nid, nodemask ? "yes" : "no",
		!ret ? "okay" : "failed");

	return ret;
}

static void dmem_test_free_all(void)
{
	struct dmem_mem_node *pos, *n;

	list_for_each_entry_safe(pos, n, &dmem_list, node) {
		list_del(&pos->node);
		dmem_free_page(__pa(pos));
	}
}

#define dmem_test_alloc(order, nid, nodemask)	\
	__dmem_test_alloc(order, nid, nodemask, __func__)

/* dmem shoud have 2^6 native pages available at lest */
static int order_test(void)
{
	int order, i, ret;
	int page_orders[] = {0, 1, 2, 3, 4, 5, 6};

	ret = dmem_test_alloc_init(PAGE_SHIFT);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(page_orders); i++) {
		order = page_orders[i];

		ret = dmem_test_alloc(order, numa_node_id(), NULL);
		if (ret)
			break;
	}

	dmem_test_free_all();

	dmem_alloc_uinit();

	return ret;
}

static int node_test(void)
{
	nodemask_t nodemask;
	unsigned long nr = 0;
	int order;
	int node;
	int ret = 0;

	order = 0;

	ret = dmem_test_alloc_init(PUD_SHIFT);
	if (ret)
		return ret;

	pr_info("%s: test allocation on node 0\n", __func__);
	node = 0;
	nodes_clear(nodemask);
	node_set(0, nodemask);

	ret = dmem_test_alloc(order, node, &nodemask);
	if (ret)
		goto exit;

	dmem_test_free_all();

	pr_info("%s: begin to exhaust dmem on node 0.\n", __func__);
	node = 1;
	nodes_clear(nodemask);
	node_set(0, nodemask);

	INIT_LIST_HEAD(&dmem_list);
	while (!(ret = dmem_test_alloc(order, node, &nodemask)))
		nr++;

	pr_info("Allocation on node 0 success times: %lu\n", nr);

	pr_info("%s: allocation on node 0 again\n", __func__);
	node = 0;
	nodes_clear(nodemask);
	node_set(0, nodemask);
	ret = dmem_test_alloc(order, node, &nodemask);
	if (!ret) {
		pr_info("\tNot expected fallback\n");
		ret = -1;
	} else {
		ret = 0;
		pr_info("\tOK, Dmem on node 0 exhausted, fallback success\n");
	}

	pr_info("%s: Release dmem\n", __func__);
	dmem_test_free_all();

exit:
	dmem_alloc_uinit();
	return ret;
}

static __init int dmem_test_init(void)
{
	int ret;

	pr_info("dmem: test init...\n");

	ret = order_test();
	if (ret)
		return ret;

	ret = node_test();


	if (ret)
		pr_info("dmem test fail, ret=%d\n", ret);
	else
		pr_info("dmem test success\n");
	return ret;
}

static __exit void dmem_test_exit(void)
{
	pr_info("dmem: test exit...\n");
}

module_init(dmem_test_init);
module_exit(dmem_test_exit);
MODULE_LICENSE("GPL v2");
