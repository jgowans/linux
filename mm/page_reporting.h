/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_PAGE_REPORTING_H
#define _MM_PAGE_REPORTING_H

#include <linux/mmzone.h>
#include <linux/pageblock-flags.h>
#include <linux/page-isolation.h>
#include <linux/jump_label.h>
#include <linux/slab.h>
#include <asm/pgtable.h>

#define PAGE_REPORTING_MIN_ORDER	pageblock_order
#define PAGE_REPORTING_HWM		32

#ifdef CONFIG_PAGE_REPORTING
/* Reported page accessors, defined in page_alloc.c */
void __free_isolated_page(struct page *page, unsigned int order);

/* Free reported_pages and reset reported page tracking count to 0 */
static inline void page_reporting_reset_zone(struct zone *zone)
{
	kfree(zone->reported_pages);
	zone->reported_pages = NULL;
}

DECLARE_STATIC_KEY_FALSE(page_reporting_enabled);
void __page_reporting_request(struct zone *zone);

static inline bool page_reported(struct page *page)
{
	return static_branch_unlikely(&page_reporting_enabled) &&
	       PageReported(page);
}

static inline unsigned long
pages_unreported(struct zone *zone, int order)
{
	unsigned long nr_free;
	int report_order;

	/* Limit notifications only to higher order pages */
	report_order = order - PAGE_REPORTING_MIN_ORDER;
	if (report_order < 0)
		return 0;

	nr_free = zone->free_area[order].nr_free;

	/* Only subtract reported_pages count if it is present */
	if (!zone->reported_pages)
		return nr_free;

	return nr_free - zone->reported_pages[report_order];
}

/**
 * page_reporting_notify_free - Free page notification to start page processing
 * @zone: Pointer to current zone of last page processed
 * @order: Order of last page added to zone
 *
 * This function is meant to act as a screener for __page_reporting_request
 * which will determine if a give zone has crossed over the high-water mark
 * that will justify us beginning page treatment. If we have crossed that
 * threshold then it will start the process of pulling some pages and
 * placing them in the batch list for treatment.
 */
static inline void page_reporting_notify_free(struct zone *zone, int order)
{
	/* Called from hot path in __free_one_page() */
	if (!static_branch_unlikely(&page_reporting_enabled))
		return;

	/* Do not bother with tests if we have already requested reporting */
	if (test_bit(ZONE_PAGE_REPORTING_REQUESTED, &zone->flags))
		return;

	/* Determine if we have crossed reporting threshold */
	if (pages_unreported(zone, order) < PAGE_REPORTING_HWM)
		return;

	/* This is slow, but should be called very rarely */
	__page_reporting_request(zone);
}

/*
 * Functions for marking/clearing reported pages from the freelist.
 * All of them expect the zone lock to be held to maintain
 * consistency of the reported_pages count versus nr_free.
 */
static inline void
mark_page_reported(struct page *page, struct zone *zone, unsigned int order)
{
	/* flag page as reported */
	__SetPageReported(page);

	/* update areated page accounting */
	zone->reported_pages[order - PAGE_REPORTING_MIN_ORDER]++;
}

static inline void
clear_page_reported(struct page *page, struct zone *zone, unsigned int order)
{
	/* page_private will contain the page order, so just use it directly */
	zone->reported_pages[order - PAGE_REPORTING_MIN_ORDER]--;

	/* clear the flag so we can report on it when it returns */
	__ClearPageReported(page);
}

#else /* CONFIG_PAGE_REPORTING */
#define page_reported(_page)	false

static inline void page_reporting_reset_zone(struct zone *zone)
{
}

static inline void page_reporting_notify_free(struct zone *zone, int order)
{
}

static inline void
clear_page_reported(struct page *page, struct zone *zone, unsigned int order)
{
}
#endif /* CONFIG_PAGE_REPORTING */
#endif /*_MM_PAGE_REPORTING_H */
