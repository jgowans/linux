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

#endif
