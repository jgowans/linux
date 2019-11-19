// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/page_reporting.h>
#include <linux/gfp.h>
#include <linux/export.h>
#include <linux/delay.h>
#include <linux/scatterlist.h>

#include "page_reporting.h"
#include "internal.h"

static struct page_reporting_dev_info __rcu *pr_dev_info __read_mostly;

#define for_each_reporting_migratetype_order(_order, _type) \
	for (_order = PAGE_REPORTING_MIN_ORDER; _order < MAX_ORDER; _order++) \
		for (_type = 0; _type < MIGRATE_TYPES; _type++) \
			if (!is_migrate_isolate(_type))

static void page_reporting_populate_metadata(struct zone *zone)
{
	size_t size;
	int node;

	/*
	 * We need to make sure we have somewhere to store the tracking
	 * data for how many reported pages are in the zone. To do that
	 * we need to make certain zone->reported_pages is populated.
	 */
	if (zone->reported_pages)
		return;

	node = zone_to_nid(zone);
	size = (MAX_ORDER - PAGE_REPORTING_MIN_ORDER) * sizeof(unsigned long);
	zone->reported_pages = kzalloc_node(size, GFP_KERNEL, node);
}

static void
page_reporting_drain(struct page_reporting_dev_info *prdev, struct zone *zone)
{
	struct scatterlist *sg = prdev->sg;

	/*
	 * Drain the now reported pages back into their respective
	 * free lists/areas. We assume at least one page is populated.
	 */
	do {
		unsigned int order = get_order(sg->length);
		struct page *page = sg_page(sg);

		__free_isolated_page(page, order);

		/*
		 * If page was not comingled with another page we can
		 * consider the result to be "reported" since the page
		 * hasn't been modified, otherwise we will need to
		 * report on the new larger page when we make our way
		 * up to that higher order.
		 */
		if (PageBuddy(page) && page_order(page) == order)
			mark_page_reported(page, zone, order);
	} while (!sg_is_last(sg++));
}

/*
 * The page reporting cycle consists of 4 stages, fill, report, drain, and
 * idle. We will cycle through the first 3 stages until we cannot obtain a
 * full scatterlist of pages, in that case we will switch to idle.
 */
static unsigned int
page_reporting_cycle(struct page_reporting_dev_info *prdev, struct zone *zone,
		     unsigned int order, unsigned int mt, unsigned int nents)
{
	struct list_head *list = &zone->free_area[order].free_list[mt];
	unsigned int page_len = PAGE_SIZE << order;
	struct scatterlist *sg = prdev->sg;
	struct page *page, *next;

	/*
	 * Perform early check, if free area is empty there is
	 * nothing to process so we can skip this free_list.
	 */
	if (list_empty(list))
		return nents;

	spin_lock_irq(&zone->lock);

	/* loop through free list adding unreported pages to sg list */
	list_for_each_entry_safe(page, next, list, lru) {
		/* We are going to skip over the reported pages. */
		if (PageReported(page))
			continue;

		/* Attempt to add page to sg list */
		if (nents < prdev->capacity) {
			if (!__isolate_free_page(page, order))
				break;

			sg_set_page(&sg[nents++], page, page_len, 0);
			continue;
		}

		/*
		 * Make the first non-reported entry in the free list
		 * the new head of the free list before we exit.
		 */
		if (!list_is_first(&page->lru, list))
			list_rotate_to_front(&page->lru, list);

		/* release lock before waiting on report processing*/
		spin_unlock_irq(&zone->lock);

		/* begin processing pages in local list */
		prdev->report(prdev, nents);

		/* reset number of entries */
		nents = 0;

		/* reacquire zone lock and resume processing free lists */
		spin_lock_irq(&zone->lock);

		/* flush reported pages from the sg list */
		page_reporting_drain(prdev, zone);

		/*
		 * Reset next to first entry, the old next isn't valid
		 * since we dropped the lock to report the pages
		 */
		next = list_first_entry(list, struct page, lru);
	}

	spin_unlock_irq(&zone->lock);

	return nents;
}

static int
page_reporting_process_zone(struct page_reporting_dev_info *prdev,
			    struct zone *zone)
{
	unsigned int order, mt, nents = 0;
	unsigned long watermark;
	int refcnt = -1;

	page_reporting_populate_metadata(zone);

	/* Generate minimum watermark to be able to guarantee progress */
	watermark = low_wmark_pages(zone) +
		    (prdev->capacity << PAGE_REPORTING_MIN_ORDER);

	/*
	 * Cancel request if insufficient free memory or if we failed
	 * to allocate page reporting statistics for the zone.
	 */
	if (!zone_watermark_ok(zone, 0, watermark, 0, ALLOC_CMA) ||
	    !zone->reported_pages) {
		spin_lock_irq(&zone->lock);
		goto zone_not_ready;
	}

	sg_init_table(prdev->sg, prdev->capacity);

	/* Process each free list starting from lowest order/mt */
	for_each_reporting_migratetype_order(order, mt)
		nents = page_reporting_cycle(prdev, zone, order, mt, nents);

	/* mark end of sg list and report the remainder */
	if (nents) {
		sg_mark_end(&prdev->sg[nents - 1]);
		prdev->report(prdev, nents);
	}

	spin_lock_irq(&zone->lock);

	/* flush any remaining pages out from the last report */
	if (nents)
		page_reporting_drain(prdev, zone);

	/* check to see if values are low enough for us to stop for now */
	for (order = PAGE_REPORTING_MIN_ORDER; order < MAX_ORDER; order++) {
		if (pages_unreported(zone, order) < PAGE_REPORTING_HWM)
			continue;
#ifdef CONFIG_MEMORY_ISOLATION
		/*
		 * Do not allow a free_area with isolated pages to request
		 * that we continue with page reporting. Keep the reporting
		 * light until the isolated pages have been cleared.
		 */
		if (!free_area_empty(&zone->free_area[order], MIGRATE_ISOLATE))
			continue;
#endif
		goto zone_not_complete;
	}

zone_not_ready:
	/*
	 * If there are no longer enough free pages to fully populate
	 * the scatterlist, then we can just shut it down for this zone.
	 */
	__clear_bit(ZONE_PAGE_REPORTING_REQUESTED, &zone->flags);
	refcnt = atomic_dec_return(&prdev->refcnt);
zone_not_complete:
	spin_unlock_irq(&zone->lock);

	return refcnt;
}

static void page_reporting_process(struct work_struct *work)
{
	struct delayed_work *d_work = to_delayed_work(work);
	struct page_reporting_dev_info *prdev =
		container_of(d_work, struct page_reporting_dev_info, work);
	struct zone *zone = first_online_pgdat()->node_zones;
	int refcnt = -1;

	do {
		if (test_bit(ZONE_PAGE_REPORTING_REQUESTED, &zone->flags))
			refcnt = page_reporting_process_zone(prdev, zone);

		/* Move to next zone, if at end of list start over */
		zone = next_zone(zone) ? : first_online_pgdat()->node_zones;

		/*
		 * As long as refcnt has not reached zero there are still
		 * zones to be processed.
		 */
	} while (refcnt);
}

/* request page reporting on this zone */
void __page_reporting_request(struct zone *zone)
{
	struct page_reporting_dev_info *prdev;

	rcu_read_lock();

	/*
	 * We use RCU to protect the pr_dev_info pointer. In almost all
	 * cases this should be present, however in the unlikely case of
	 * a shutdown this will be NULL and we should exit.
	 */
	prdev = rcu_dereference(pr_dev_info);
	if (unlikely(!prdev))
		goto out;

	/*
	 * We can use separate test and set operations here as there
	 * is nothing else that can set or clear this bit while we are
	 * holding the zone lock. The advantage to doing it this way is
	 * that we don't have to dirty the cacheline unless we are
	 * changing the value.
	 */
	__set_bit(ZONE_PAGE_REPORTING_REQUESTED, &zone->flags);

	/*
	 * Delay the start of work to allow a sizable queue to
	 * build. For now we are limiting this to running no more
	 * than 5 times per second.
	 */
	if (!atomic_fetch_inc(&prdev->refcnt))
		schedule_delayed_work(&prdev->work, HZ / 5);
out:
	rcu_read_unlock();
}

static DEFINE_MUTEX(page_reporting_mutex);
DEFINE_STATIC_KEY_FALSE(page_reporting_enabled);

void page_reporting_unregister(struct page_reporting_dev_info *prdev)
{
	mutex_lock(&page_reporting_mutex);

	if (rcu_access_pointer(pr_dev_info) == prdev) {
		/* Disable page reporting notification */
		RCU_INIT_POINTER(pr_dev_info, NULL);
		synchronize_rcu();

		/* Flush any existing work, and lock it out */
		cancel_delayed_work_sync(&prdev->work);

		/* Free scatterlist */
		kfree(prdev->sg);
		prdev->sg = NULL;
	}

	mutex_unlock(&page_reporting_mutex);
}
EXPORT_SYMBOL_GPL(page_reporting_unregister);

int page_reporting_register(struct page_reporting_dev_info *prdev)
{
	struct zone *zone;
	int err = 0;

	/* No point in enabling this if it cannot handle any pages */
	if (WARN_ON(!prdev->capacity || prdev->capacity > PAGE_REPORTING_HWM))
		return -EINVAL;

	mutex_lock(&page_reporting_mutex);

	/* nothing to do if already in use */
	if (rcu_access_pointer(pr_dev_info)) {
		err = -EBUSY;
		goto err_out;
	}

	/* allocate scatterlist to store pages being reported on */
	prdev->sg = kcalloc(prdev->capacity, sizeof(*prdev->sg), GFP_KERNEL);
	if (!prdev->sg) {
		err = -ENOMEM;
		goto err_out;
	}


	/* initialize refcnt and work structures */
	atomic_set(&prdev->refcnt, 0);
	INIT_DELAYED_WORK(&prdev->work, &page_reporting_process);

	/* assign device, and begin initial flush of populated zones */
	rcu_assign_pointer(pr_dev_info, prdev);
	for_each_populated_zone(zone) {
		spin_lock_irq(&zone->lock);
		__page_reporting_request(zone);
		spin_unlock_irq(&zone->lock);
	}

	/* enable page reporting notification */
	if (!static_key_enabled(&page_reporting_enabled)) {
		static_branch_enable(&page_reporting_enabled);
		pr_info("Unused page reporting enabled\n");
	}
err_out:
	mutex_unlock(&page_reporting_mutex);

	return err;
}
EXPORT_SYMBOL_GPL(page_reporting_register);
