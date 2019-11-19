/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_REPORTING_H
#define _LINUX_PAGE_REPORTING_H

#include <linux/mmzone.h>

struct page_reporting_dev_info {
	/* function that alters pages to make them "reported" */
	void (*report)(struct page_reporting_dev_info *prdev,
		       unsigned int nents);

	/* pointer to scatterlist containing pages to be processed */
	struct scatterlist *sg;

	/*
	 * Upper limit on the number of pages that the report function
	 * expects to be placed into the scatterlist to be processed.
	 */
	unsigned int capacity;

	/* The number of zones requesting reporting */
	atomic_t refcnt;

	/* work struct for processing reports */
	struct delayed_work work;
};

/* Tear-down and bring-up for page reporting devices */
void page_reporting_unregister(struct page_reporting_dev_info *prdev);
int page_reporting_register(struct page_reporting_dev_info *prdev);
#endif /*_LINUX_PAGE_REPORTING_H */
