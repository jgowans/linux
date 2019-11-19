.. _unused_page_reporting:

=====================
Unused Page Reporting
=====================

Unused page reporting is an API by which a device can register to receive
lists of pages that are currently unused by the system. This is useful in
the case of virtualization where a guest is then able to use this data to
notify the hypervisor that it is no longer using certain pages in memory.

For the driver, typically a balloon driver, to use of this functionality
it will allocate and initialize a page_reporting_dev_info structure. The
fields within the structure it will populate are the "report" function
pointer used to process the scatterlist and "capacity" representing the
number of entries that the device can support in a single request. Once
those are populated a call to page_reporting_register will allocate the
scatterlist and register the device with the reporting framework assuming
no other page reporting devices are already registered.

Once registered the page reporting API will begin reporting batches of
pages to the driver. The API determines that it needs to start reporting by
measuring the number of pages in a given free area versus the number of
reported pages for that free area. If the value meets or exceeds the value
defined by PAGE_REPORTING_HWM then the zone is flagged as requesting
reporting and a worker is scheduled to process zone requesting reporting.

Pages reported will be stored in the scatterlist pointed to in the
page_reporting_dev_info with the final entry having the end bit set in
entry nent - 1. While pages are being processed by the report function they
will not be accessible to the allocator. Once the report function has been
completed the pages will be returned to the free area from which they were
obtained.

Prior to removing a driver that is making use of unused page reporting it
is necessary to call page_reporting_unregister to have the
page_reporting_dev_info structure that is currently in use by unused page
reporting removed. Doing this will prevent further reports from being
issued via the interface. If another driver or the same driver is
registered it is possible for it to resume where the previous driver had
left off in terms of reporting unused pages.

Alexander Duyck, Nov 15, 2019
