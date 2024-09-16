/* SPDX-License-Identifier: MIT */

#ifndef _LINUX_GUESTMEMFS_H
#define _LINUX_GUESTMEMFS_H

#include <linux/fs.h>

/*
 * Carves out chunks of memory from memblocks for guestmemfs.
 * Must be called in early boot before memblocks are freed.
 */
# ifdef CONFIG_GUESTMEMFS_FS
void guestmemfs_reserve_mem(void);
bool is_guestmemfs_file(struct file const *filp);
#else
void guestmemfs_reserve_mem(void) { }
inline bool is_guestmemfs_file(struct file const *filp)
{
	return 0;
}
#endif

/*
 * Ensure that the file cannot be deleted or have its memory changed
 * until it is unpinned. The returned value is a handle which can be
 * used to un-pin the file.
 */
unsigned long guestmemfs_pin_file(struct file *file);
void guestmemfs_unpin_file(unsigned long pin_handle);

#endif
